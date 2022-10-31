/*
 * Strawberry Music Player
 * Copyright 2019-2021, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QImage>
#include <QImageReader>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QTimer>

#include "core/logging.h"
#include "core/networkaccessmanager.h"
#include "core/song.h"
#include "core/timeconstants.h"
#include "core/application.h"
#include "core/imageutils.h"
#include "qobuzservice.h"
#include "qobuzurlhandler.h"
#include "qobuzbaserequest.h"
#include "qobuzrequest.h"

constexpr int QobuzRequest::kMaxConcurrentArtistsRequests = 3;
constexpr int QobuzRequest::kMaxConcurrentAlbumsRequests = 3;
constexpr int QobuzRequest::kMaxConcurrentSongsRequests = 3;
constexpr int QobuzRequest::kMaxConcurrentArtistAlbumsRequests = 3;
constexpr int QobuzRequest::kMaxConcurrentAlbumSongsRequests = 3;
constexpr int QobuzRequest::kMaxConcurrentAlbumCoverRequests = 1;
constexpr int QobuzRequest::kFlushRequestsDelay = 200;

QobuzRequest::QobuzRequest(QobuzService *service, QobuzUrlHandler *url_handler, Application *app, NetworkAccessManager *network, QueryType type, QObject *parent)
    : QobuzBaseRequest(service, network, parent),
      service_(service),
      url_handler_(url_handler),
      app_(app),
      network_(network),
      timer_flush_requests_(new QTimer(this)),
      type_(type),
      query_id_(-1),
      finished_(false),
      artists_requests_total_(0),
      artists_requests_active_(0),
      artists_requests_received_(0),
      artists_total_(0),
      artists_received_(0),
      albums_requests_total_(0),
      albums_requests_active_(0),
      albums_requests_received_(0),
      albums_total_(0),
      albums_received_(0),
      songs_requests_total_(0),
      songs_requests_active_(0),
      songs_requests_received_(0),
      songs_total_(0),
      songs_received_(0),
      artist_albums_requests_total_(),
      artist_albums_requests_active_(0),
      artist_albums_requests_received_(0),
      artist_albums_total_(0),
      artist_albums_received_(0),
      album_songs_requests_active_(0),
      album_songs_requests_received_(0),
      album_songs_requests_total_(0),
      album_songs_total_(0),
      album_songs_received_(0),
      album_covers_requests_total_(0),
      album_covers_requests_active_(0),
      album_covers_requests_received_(0),
      no_results_(false) {

  timer_flush_requests_->setInterval(kFlushRequestsDelay);
  timer_flush_requests_->setSingleShot(false);
  QObject::connect(timer_flush_requests_, &QTimer::timeout, this, &QobuzRequest::FlushRequests);

}

QobuzRequest::~QobuzRequest() {

  while (!replies_.isEmpty()) {
    QNetworkReply *reply = replies_.takeFirst();
    QObject::disconnect(reply, nullptr, this, nullptr);
    if (reply->isRunning()) reply->abort();
    reply->deleteLater();
  }

  while (!album_cover_replies_.isEmpty()) {
    QNetworkReply *reply = album_cover_replies_.takeFirst();
    QObject::disconnect(reply, nullptr, this, nullptr);
    if (reply->isRunning()) reply->abort();
    reply->deleteLater();
  }

}

void QobuzRequest::Process() {

  switch (type_) {
    case QueryType::QueryType_Artists:
      GetArtists();
      break;
    case QueryType::QueryType_Albums:
      GetAlbums();
      break;
    case QueryType::QueryType_Songs:
      GetSongs();
      break;
    case QueryType::QueryType_SearchArtists:
      ArtistsSearch();
      break;
    case QueryType::QueryType_SearchAlbums:
      AlbumsSearch();
      break;
    case QueryType::QueryType_SearchSongs:
      SongsSearch();
      break;
    default:
      Error("Invalid query type.");
      break;
  }

}

void QobuzRequest::StartRequests() {

  if (!timer_flush_requests_->isActive()) {
    timer_flush_requests_->start();
  }

}

void QobuzRequest::FlushRequests() {

  if (!artists_requests_queue_.isEmpty()) {
    FlushArtistsRequests();
    return;
  }

  if (!albums_requests_queue_.isEmpty()) {
    FlushAlbumsRequests();
    return;
  }

  if (!artist_albums_requests_queue_.isEmpty()) {
    FlushArtistAlbumsRequests();
    return;
  }

  if (!album_songs_requests_queue_.isEmpty()) {
    FlushAlbumSongsRequests();
    return;
  }

  if (!songs_requests_queue_.isEmpty()) {
    FlushSongsRequests();
    return;
  }

  if (!album_cover_requests_queue_.isEmpty()) {
    FlushAlbumCoverRequests();
    return;
  }

  timer_flush_requests_->stop();

}

void QobuzRequest::Search(const int query_id, const QString &search_text) {
  query_id_ = query_id;
  search_text_ = search_text;
}

void QobuzRequest::GetArtists() {

  emit UpdateStatus(query_id_, tr("Receiving artists..."));
  emit UpdateProgress(query_id_, 0);
  AddArtistsRequest();

}

void QobuzRequest::AddArtistsRequest(const int offset, const int limit) {

  Request request;
  request.limit = limit;
  request.offset = offset;
  artists_requests_queue_.enqueue(request);

  ++artists_requests_total_;

  StartRequests();

}

void QobuzRequest::FlushArtistsRequests() {

  while (!artists_requests_queue_.isEmpty() && artists_requests_active_ < kMaxConcurrentArtistsRequests) {

    Request request = artists_requests_queue_.dequeue();

    ParamList params;
    if (type_ == QueryType_Artists) {
      params << Param("type", "artists");
      params << Param("user_auth_token", user_auth_token());
    }
    else if (type_ == QueryType_SearchArtists) params << Param("query", search_text_);
    if (request.limit > 0) params << Param("limit", QString::number(request.limit));
    if (request.offset > 0) params << Param("offset", QString::number(request.offset));
    QNetworkReply *reply = nullptr;
    if (type_ == QueryType_Artists) {
      reply = CreateRequest(QString("favorite/getUserFavorites"), params);
    }
    else if (type_ == QueryType_SearchArtists) {
      reply = CreateRequest("artist/search", params);
    }
    if (!reply) continue;
    replies_ << reply;
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, request]() { ArtistsReplyReceived(reply, request.limit, request.offset); });

    ++artists_requests_active_;

  }

}

void QobuzRequest::GetAlbums() {

  emit UpdateStatus(query_id_, tr("Receiving albums..."));
  emit UpdateProgress(query_id_, 0);
  AddAlbumsRequest();

}

void QobuzRequest::AddAlbumsRequest(const int offset, const int limit) {

  Request request;
  request.limit = limit;
  request.offset = offset;
  albums_requests_queue_.enqueue(request);

  ++albums_requests_total_;

  StartRequests();

}

void QobuzRequest::FlushAlbumsRequests() {

  while (!albums_requests_queue_.isEmpty() && albums_requests_active_ < kMaxConcurrentAlbumsRequests) {

    Request request = albums_requests_queue_.dequeue();

    ParamList params;
    if (type_ == QueryType_Albums) {
      params << Param("type", "albums");
      params << Param("user_auth_token", user_auth_token());
    }
    else if (type_ == QueryType_SearchAlbums) params << Param("query", search_text_);
    if (request.limit > 0) params << Param("limit", QString::number(request.limit));
    if (request.offset > 0) params << Param("offset", QString::number(request.offset));
    QNetworkReply *reply = nullptr;
    if (type_ == QueryType_Albums) {
      reply = CreateRequest(QString("favorite/getUserFavorites"), params);
    }
    else if (type_ == QueryType_SearchAlbums) {
      reply = CreateRequest("album/search", params);
    }
    if (!reply) continue;
    replies_ << reply;
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, request]() { AlbumsReplyReceived(reply, request.limit, request.offset); });

    ++albums_requests_active_;

  }

}

void QobuzRequest::GetSongs() {

  emit UpdateStatus(query_id_, tr("Receiving songs..."));
  emit UpdateProgress(query_id_, 0);
  AddSongsRequest();

}

void QobuzRequest::AddSongsRequest(const int offset, const int limit) {

  Request request;
  request.limit = limit;
  request.offset = offset;
  songs_requests_queue_.enqueue(request);

  ++songs_requests_total_;

  StartRequests();

}

void QobuzRequest::FlushSongsRequests() {

  while (!songs_requests_queue_.isEmpty() && songs_requests_active_ < kMaxConcurrentSongsRequests) {

    Request request = songs_requests_queue_.dequeue();

    ParamList params;
    if (type_ == QueryType_Songs) {
      params << Param("type", "tracks");
      params << Param("user_auth_token", user_auth_token());
    }
    else if (type_ == QueryType_SearchSongs) params << Param("query", search_text_);
    if (request.limit > 0) params << Param("limit", QString::number(request.limit));
    if (request.offset > 0) params << Param("offset", QString::number(request.offset));
    QNetworkReply *reply = nullptr;
    if (type_ == QueryType_Songs) {
      reply = CreateRequest(QString("favorite/getUserFavorites"), params);
    }
    else if (type_ == QueryType_SearchSongs) {
      reply = CreateRequest("track/search", params);
    }
    if (!reply) continue;
    replies_ << reply;
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, request]() { SongsReplyReceived(reply, request.limit, request.offset); });

    ++songs_requests_active_;

  }

}

void QobuzRequest::ArtistsSearch() {

  emit UpdateStatus(query_id_, tr("Searching..."));
  emit UpdateProgress(query_id_, 0);
  AddArtistsSearchRequest();

}

void QobuzRequest::AddArtistsSearchRequest(const int offset) {

  AddArtistsRequest(offset, service_->artistssearchlimit());

}

void QobuzRequest::AlbumsSearch() {

  emit UpdateStatus(query_id_, tr("Searching..."));
  emit UpdateProgress(query_id_, 0);
  AddAlbumsSearchRequest();

}

void QobuzRequest::AddAlbumsSearchRequest(const int offset) {

  AddAlbumsRequest(offset, service_->albumssearchlimit());

}

void QobuzRequest::SongsSearch() {

  emit UpdateStatus(query_id_, tr("Searching..."));
  emit UpdateProgress(query_id_, 0);
  AddSongsSearchRequest();

}

void QobuzRequest::AddSongsSearchRequest(const int offset) {

  AddSongsRequest(offset, service_->songssearchlimit());

}

void QobuzRequest::ArtistsReplyReceived(QNetworkReply *reply, const int limit_requested, const int offset_requested) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  QByteArray data = GetReplyData(reply);

  --artists_requests_active_;
  ++artists_requests_received_;

  if (finished_) return;

  if (data.isEmpty()) {
    ArtistsFinishCheck();
    return;
  }

  QJsonObject json_obj = ExtractJsonObj(data);
  if (json_obj.isEmpty()) {
    ArtistsFinishCheck();
    return;
  }

  if (!json_obj.contains("artists")) {
    ArtistsFinishCheck();
    Error("Json object is missing artists.", json_obj);
    return;
  }
  QJsonValue value_artists = json_obj["artists"];
  if (!value_artists.isObject()) {
    Error("Json artists is not an object.", json_obj);
    ArtistsFinishCheck();
    return;
  }
  QJsonObject obj_artists = value_artists.toObject();

  if (!obj_artists.contains("limit") ||
      !obj_artists.contains("offset") ||
      !obj_artists.contains("total") ||
      !obj_artists.contains("items")) {
    ArtistsFinishCheck();
    Error("Json artists object is missing values.", json_obj);
    return;
  }
  //int limit = obj_artists["limit"].toInt();
  int offset = obj_artists["offset"].toInt();
  int artists_total = obj_artists["total"].toInt();

  if (offset_requested == 0) {
    artists_total_ = artists_total;
  }
  else if (artists_total != artists_total_) {
    Error(QString("total returned does not match previous total! %1 != %2").arg(artists_total).arg(artists_total_));
    ArtistsFinishCheck();
    return;
  }

  if (offset != offset_requested) {
    Error(QString("Offset returned does not match offset requested! %1 != %2").arg(offset).arg(offset_requested));
    ArtistsFinishCheck();
    return;
  }

  if (offset_requested == 0) {
    emit UpdateProgress(query_id_, GetProgress(artists_received_, artists_total_));
  }

  QJsonValue value_items = ExtractItems(obj_artists);
  if (!value_items.isArray()) {
    ArtistsFinishCheck();
    return;
  }

  QJsonArray array_items = value_items.toArray();
  if (array_items.isEmpty()) {  // Empty array means no results
    if (offset_requested == 0) no_results_ = true;
    ArtistsFinishCheck();
    return;
  }

  int artists_received = 0;
  for (const QJsonValueRef value_item : array_items) {

    ++artists_received;

    if (!value_item.isObject()) {
      Error("Invalid Json reply, item not a object.");
      continue;
    }
    QJsonObject obj_item = value_item.toObject();

    if (obj_item.contains("item")) {
      QJsonValue json_item = obj_item["item"];
      if (!json_item.isObject()) {
        Error("Invalid Json reply, item not a object.", json_item);
        continue;
      }
      obj_item = json_item.toObject();
    }

    if (!obj_item.contains("id") || !obj_item.contains("name")) {
      Error("Invalid Json reply, item missing id or album.", obj_item);
      continue;
    }

    Artist artist;
    if (obj_item["id"].isString()) {
      artist.artist_id = obj_item["id"].toString();
    }
    else {
      artist.artist_id = QString::number(obj_item["id"].toInt());
    }
    artist.artist = obj_item["name"].toString();

    if (artist_albums_requests_pending_.contains(artist.artist_id)) continue;

    ArtistAlbumsRequest request;
    request.artist = artist;
    artist_albums_requests_pending_.insert(artist.artist_id, request);

  }
  artists_received_ += artists_received;

  if (offset_requested != 0) emit UpdateProgress(query_id_, GetProgress(artists_received_, artists_total_));

  ArtistsFinishCheck(limit_requested, offset, artists_received);

}

void QobuzRequest::ArtistsFinishCheck(const int limit, const int offset, const int artists_received) {

  if (finished_) return;

  if ((limit == 0 || limit > artists_received) && artists_received_ < artists_total_) {
    int offset_next = offset + artists_received;
    if (offset_next > 0 && offset_next < artists_total_) {
      if (type_ == QueryType_Artists) AddArtistsRequest(offset_next);
      else if (type_ == QueryType_SearchArtists) AddArtistsSearchRequest(offset_next);
    }
  }

  if (artists_requests_queue_.isEmpty() && artists_requests_active_ <= 0) {  // Artist query is finished, get all albums for all artists.

    // Get artist albums
    QList<ArtistAlbumsRequest> requests = artist_albums_requests_pending_.values();
    for (const ArtistAlbumsRequest &request : requests) {
      AddArtistAlbumsRequest(request.artist);
    }
    artist_albums_requests_pending_.clear();

    if (artist_albums_requests_total_ > 0) {
      if (artist_albums_requests_total_ == 1) emit UpdateStatus(query_id_, tr("Receiving albums for %1 artist...").arg(artist_albums_requests_total_));
      else emit UpdateStatus(query_id_, tr("Receiving albums for %1 artists...").arg(artist_albums_requests_total_));
      emit UpdateProgress(query_id_, 0);
    }

  }

  FinishCheck();

}

void QobuzRequest::AlbumsReplyReceived(QNetworkReply *reply, const int limit_requested, const int offset_requested) {

  --albums_requests_active_;
  ++albums_requests_received_;
  AlbumsReceived(reply, Artist(), limit_requested, offset_requested);

}

void QobuzRequest::AddArtistAlbumsRequest(const Artist &artist, const int offset) {

  ArtistAlbumsRequest request;
  request.artist = artist;
  request.offset = offset;
  artist_albums_requests_queue_.enqueue(request);

  ++artist_albums_requests_total_;

  StartRequests();

}

void QobuzRequest::FlushArtistAlbumsRequests() {

  while (!artist_albums_requests_queue_.isEmpty() && artist_albums_requests_active_ < kMaxConcurrentArtistAlbumsRequests) {

    const ArtistAlbumsRequest request = artist_albums_requests_queue_.dequeue();

    ParamList params = ParamList() << Param("artist_id", request.artist.artist_id)
                                   << Param("extra", "albums");

    if (request.offset > 0) params << Param("offset", QString::number(request.offset));
    QNetworkReply *reply = CreateRequest(QString("artist/get"), params);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, request]() { ArtistAlbumsReplyReceived(reply, request.artist, request.offset); });
    replies_ << reply;

    ++artist_albums_requests_active_;

  }

}

void QobuzRequest::ArtistAlbumsReplyReceived(QNetworkReply *reply, const Artist &artist, const int offset_requested) {

  --artist_albums_requests_active_;
  ++artist_albums_requests_received_;
  emit UpdateProgress(query_id_, GetProgress(artist_albums_requests_received_, artist_albums_requests_total_));
  AlbumsReceived(reply, artist, 0, offset_requested);

}

void QobuzRequest::AlbumsReceived(QNetworkReply *reply, const Artist &artist_requested, const int limit_requested, const int offset_requested) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  QByteArray data = GetReplyData(reply);

  if (finished_) return;

  if (data.isEmpty()) {
    AlbumsFinishCheck(artist_requested);
    return;
  }

  QJsonObject json_obj = ExtractJsonObj(data);
  if (json_obj.isEmpty()) {
    AlbumsFinishCheck(artist_requested);
    return;
  }

  Artist artist = artist_requested;

  if (json_obj.contains("id") && json_obj.contains("name")) {
    if (json_obj["id"].isString()) {
      artist.artist_id = json_obj["id"].toString();
    }
    else {
      artist.artist_id = QString::number(json_obj["id"].toInt());
    }
    artist.artist = json_obj["name"].toString();
  }

  if (artist.artist_id != artist_requested.artist_id) {
    AlbumsFinishCheck(artist_requested);
    Error("Artist ID returned does not match artist ID requested.", json_obj);
    return;
  }

  if (!json_obj.contains("albums")) {
    AlbumsFinishCheck(artist_requested);
    Error("Json object is missing albums.", json_obj);
    return;
  }
  QJsonValue value_albums = json_obj["albums"];
  if (!value_albums.isObject()) {
    Error("Json albums is not an object.", json_obj);
    AlbumsFinishCheck(artist_requested);
    return;
  }
  QJsonObject obj_albums = value_albums.toObject();

  if (!obj_albums.contains("limit") ||
      !obj_albums.contains("offset") ||
      !obj_albums.contains("total") ||
      !obj_albums.contains("items")) {
    AlbumsFinishCheck(artist_requested);
    Error("Json albums object is missing values.", json_obj);
    return;
  }

  //int limit = obj_albums["limit"].toInt();
  int offset = obj_albums["offset"].toInt();
  int albums_total = obj_albums["total"].toInt();

  if (offset != offset_requested) {
    Error(QString("Offset returned does not match offset requested! %1 != %2").arg(offset).arg(offset_requested));
    AlbumsFinishCheck(artist_requested);
    return;
  }

  QJsonValue value_items = ExtractItems(obj_albums);
  if (!value_items.isArray()) {
    AlbumsFinishCheck(artist_requested);
    return;
  }
  QJsonArray array_items = value_items.toArray();
  if (array_items.isEmpty()) {
    if ((type_ == QueryType_Albums || type_ == QueryType_SearchAlbums) && offset_requested == 0) {
      no_results_ = true;
    }
    AlbumsFinishCheck(artist_requested);
    return;
  }

  int albums_received = 0;
  for (const QJsonValueRef value_item : array_items) {

    ++albums_received;

    if (!value_item.isObject()) {
      Error("Invalid Json reply, item in array is not a object.");
      continue;
    }
    QJsonObject obj_item = value_item.toObject();

    if (!obj_item.contains("artist") || !obj_item.contains("title") || !obj_item.contains("id")) {
      Error("Invalid Json reply, item missing artist, title or id.", obj_item);
      continue;
    }

    Album album;
    if (obj_item["id"].isString()) {
      album.album_id = obj_item["id"].toString();
    }
    else {
      album.album_id = QString::number(obj_item["id"].toInt());
    }
    album.album = obj_item["title"].toString();

    if (album_songs_requests_pending_.contains(album.album_id)) continue;

    QJsonValue value_artist = obj_item["artist"];
    if (!value_artist.isObject()) {
      Error("Invalid Json reply, item artist is not a object.", value_artist);
      continue;
    }
    QJsonObject obj_artist = value_artist.toObject();
    if (!obj_artist.contains("id") || !obj_artist.contains("name")) {
      Error("Invalid Json reply, item artist missing id or name.", obj_artist);
      continue;
    }

    Artist album_artist;
    if (obj_artist["id"].isString()) {
      album_artist.artist_id = obj_artist["id"].toString();
    }
    else {
      album_artist.artist_id = QString::number(obj_artist["id"].toInt());
    }
    album_artist.artist = obj_artist["name"].toString();

    if (!artist_requested.artist_id.isEmpty() && album_artist.artist_id != artist_requested.artist_id) {
      qLog(Debug) << "Skipping artist" << album_artist.artist << album_artist.artist_id << "does not match album artist" << artist_requested.artist_id << artist_requested.artist;
      continue;
    }

    AlbumSongsRequest request;
    request.artist = album_artist;
    request.album = album;
    album_songs_requests_pending_.insert(album.album_id, request);

  }

  if (type_ == QueryType_Albums || type_ == QueryType_SearchAlbums) {
    albums_received_ += albums_received;
    emit UpdateProgress(query_id_, GetProgress(albums_received_, albums_total_));
  }

  AlbumsFinishCheck(artist_requested, limit_requested, offset, albums_total, albums_received);

}

void QobuzRequest::AlbumsFinishCheck(const Artist &artist, const int limit, const int offset, const int albums_total, const int albums_received) {

  if (finished_) return;

  if (limit == 0 || limit > albums_received) {
    int offset_next = offset + albums_received;
    if (offset_next > 0 && offset_next < albums_total) {
      switch (type_) {
        case QueryType_Albums:
          AddAlbumsRequest(offset_next);
          break;
        case QueryType_SearchAlbums:
          AddAlbumsSearchRequest(offset_next);
          break;
        case QueryType_Artists:
        case QueryType_SearchArtists:
          AddArtistAlbumsRequest(artist, offset_next);
          break;
        default:
          break;
      }
    }
  }

  if (
      artists_requests_queue_.isEmpty() &&
      artists_requests_active_ <= 0 &&
      albums_requests_queue_.isEmpty() &&
      albums_requests_active_ <= 0 &&
      artist_albums_requests_queue_.isEmpty() &&
      artist_albums_requests_active_ <= 0
      ) { // Artist albums query is finished, get all songs for all albums.

    // Get songs for all the albums.

    for (QHash<QString, AlbumSongsRequest>::iterator it = album_songs_requests_pending_.begin(); it != album_songs_requests_pending_.end(); ++it) {
      const AlbumSongsRequest &request = it.value();
      AddAlbumSongsRequest(request.artist, request.album);
    }
    album_songs_requests_pending_.clear();

    if (album_songs_requests_total_ > 0) {
      if (album_songs_requests_total_ == 1) emit UpdateStatus(query_id_, tr("Receiving songs for %1 album...").arg(album_songs_requests_total_));
      else emit UpdateStatus(query_id_, tr("Receiving songs for %1 albums...").arg(album_songs_requests_total_));
      emit UpdateProgress(query_id_, 0);
    }
  }

  GetAlbumCoversCheck();
  FinishCheck();

}

void QobuzRequest::SongsReplyReceived(QNetworkReply *reply, const int limit_requested, const int offset_requested) {

  --songs_requests_active_;
  ++songs_requests_received_;
  SongsReceived(reply, Artist(), Album(), limit_requested, offset_requested);

}

void QobuzRequest::AddAlbumSongsRequest(const Artist &artist, const Album &album, const int offset) {

  AlbumSongsRequest request;
  request.artist = artist;
  request.album = album;
  request.offset = offset;
  album_songs_requests_queue_.enqueue(request);

  ++album_songs_requests_total_;

  StartRequests();

}

void QobuzRequest::FlushAlbumSongsRequests() {

  while (!album_songs_requests_queue_.isEmpty() && album_songs_requests_active_ < kMaxConcurrentAlbumSongsRequests) {

    AlbumSongsRequest request = album_songs_requests_queue_.dequeue();
    ParamList params = ParamList() << Param("album_id", request.album.album_id);
    if (request.offset > 0) params << Param("offset", QString::number(request.offset));
    QNetworkReply *reply = CreateRequest(QString("album/get"), params);
    replies_ << reply;
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, request]() { AlbumSongsReplyReceived(reply, request.artist, request.album, request.offset); });

    ++album_songs_requests_active_;

  }

}

void QobuzRequest::AlbumSongsReplyReceived(QNetworkReply *reply, const Artist &artist, const Album &album, const int offset_requested) {

  --album_songs_requests_active_;
  ++album_songs_requests_received_;
  if (offset_requested == 0) {
    emit UpdateProgress(query_id_, GetProgress(album_songs_requests_received_, album_songs_requests_total_));
  }
  SongsReceived(reply, artist, album, 0, offset_requested);

}

void QobuzRequest::SongsReceived(QNetworkReply *reply, const Artist &artist_requested, const Album &album_requested, const int limit_requested, const int offset_requested) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  QByteArray data = GetReplyData(reply);

  if (finished_) return;

  if (data.isEmpty()) {
    SongsFinishCheck(artist_requested, album_requested, limit_requested, offset_requested);
    return;
  }

  QJsonObject json_obj = ExtractJsonObj(data);
  if (json_obj.isEmpty()) {
    SongsFinishCheck(artist_requested, album_requested, limit_requested, offset_requested);
    return;
  }

  if (!json_obj.contains("tracks")) {
    Error("Json object is missing tracks.", json_obj);
    SongsFinishCheck(artist_requested, album_requested, limit_requested, offset_requested);
    return;
  }

  Artist album_artist = artist_requested;
  Album album = album_requested;

  if (json_obj.contains("id") && json_obj.contains("title")) {
    if (json_obj["id"].isString()) {
      album.album_id = json_obj["id"].toString();
    }
    else {
      album.album_id = QString::number(json_obj["id"].toInt());
    }
    album.album = json_obj["title"].toString();
  }

  if (json_obj.contains("artist")) {
    QJsonValue value_artist = json_obj["artist"];
    if (!value_artist.isObject()) {
      Error("Invalid Json reply, album artist is not a object.", value_artist);
      SongsFinishCheck(artist_requested, album_requested, limit_requested, offset_requested);
      return;
    }
    QJsonObject obj_artist = value_artist.toObject();
    if (!obj_artist.contains("id") || !obj_artist.contains("name")) {
      Error("Invalid Json reply, album artist is missing id or name.", obj_artist);
      SongsFinishCheck(artist_requested, album_requested, limit_requested, offset_requested);
      return;
    }
    if (obj_artist["id"].isString()) {
      album_artist.artist_id = obj_artist["id"].toString();
    }
    else {
      album_artist.artist_id = QString::number(obj_artist["id"].toInt());
    }
    album_artist.artist = obj_artist["name"].toString();
  }

  if (json_obj.contains("image")) {
    QJsonValue value_image = json_obj["image"];
    if (!value_image.isObject()) {
      Error("Invalid Json reply, album image is not a object.", value_image);
      SongsFinishCheck(artist_requested, album_requested, limit_requested, offset_requested);
      return;
    }
    QJsonObject obj_image = value_image.toObject();
    if (!obj_image.contains("large")) {
      Error("Invalid Json reply, album image is missing large.", obj_image);
      SongsFinishCheck(artist_requested, album_requested, limit_requested, offset_requested);
      return;
    }
    QString album_image = obj_image["large"].toString();
    if (!album_image.isEmpty()) {
      album.cover_url = QUrl(album_image);
    }
  }

  QJsonValue value_tracks = json_obj["tracks"];
  if (!value_tracks.isObject()) {
    Error("Json tracks is not an object.", json_obj);
    SongsFinishCheck(artist_requested, album_requested, limit_requested, offset_requested);
    return;
  }
  QJsonObject obj_tracks = value_tracks.toObject();

  if (!obj_tracks.contains("limit") ||
      !obj_tracks.contains("offset") ||
      !obj_tracks.contains("total") ||
      !obj_tracks.contains("items")) {
    SongsFinishCheck(artist_requested, album_requested, limit_requested, offset_requested);
    Error("Json songs object is missing values.", json_obj);
    return;
  }

  //int limit = obj_tracks["limit"].toInt();
  int offset = obj_tracks["offset"].toInt();
  int songs_total = obj_tracks["total"].toInt();

  if (offset != offset_requested) {
    Error(QString("Offset returned does not match offset requested! %1 != %2").arg(offset).arg(offset_requested));
    SongsFinishCheck(album_artist, album, limit_requested, offset_requested, songs_total);
    return;
  }

  QJsonValue value_items = ExtractItems(obj_tracks);
  if (!value_items.isArray()) {
    SongsFinishCheck(album_artist, album, limit_requested, offset_requested, songs_total);
    return;
  }

  QJsonArray array_items = value_items.toArray();
  if (array_items.isEmpty()) {
    if ((type_ == QueryType_Songs || type_ == QueryType_SearchSongs) && offset_requested == 0) {
      no_results_ = true;
    }
    SongsFinishCheck(album_artist, album, limit_requested, offset_requested, songs_total);
    return;
  }

  bool compilation = false;
  bool multidisc = false;
  SongList songs;
  int songs_received = 0;
  for (const QJsonValueRef value_item : array_items) {

    if (!value_item.isObject()) {
      Error("Invalid Json reply, track is not a object.");
      continue;
    }
    QJsonObject obj_item = value_item.toObject();

    ++songs_received;
    Song song(Song::Source_Qobuz);
    ParseSong(song, obj_item, album_artist, album);
    if (!song.is_valid()) continue;
    if (song.disc() >= 2) multidisc = true;
    if (song.is_compilation()) compilation = true;
    songs << song;
  }

  for (Song song : songs) {
    if (compilation) song.set_compilation_detected(true);
    if (!multidisc) song.set_disc(0);
    songs_.insert(song.song_id(), song);
  }

  if (type_ == QueryType_Songs || type_ == QueryType_SearchSongs) {
    songs_received_ += songs_received;
    emit UpdateProgress(query_id_, GetProgress(songs_received_, songs_total_));
  }

  SongsFinishCheck(album_artist, album, limit_requested, offset_requested, songs_total, songs_received);

}

void QobuzRequest::SongsFinishCheck(const Artist &artist, const Album &album, const int limit, const int offset, const int songs_total, const int songs_received) {

  if (finished_) return;

  if (limit == 0 || limit > songs_received) {
    int offset_next = offset + songs_received;
    if (offset_next > 0 && offset_next < songs_total) {
      switch (type_) {
        case QueryType_Songs:
          AddSongsRequest(offset_next);
          break;
        case QueryType_SearchSongs:
          AddSongsSearchRequest(offset_next);
          break;
        case QueryType_Artists:
        case QueryType_SearchArtists:
        case QueryType_Albums:
        case QueryType_SearchAlbums:
          AddAlbumSongsRequest(artist, album, offset_next);
          break;
        default:
          break;
      }
    }
  }

  GetAlbumCoversCheck();
  FinishCheck();

}

void QobuzRequest::ParseSong(Song &song, const QJsonObject &json_obj, const Artist &album_artist, const Album &album) {

  if (
      !json_obj.contains("id") ||
      !json_obj.contains("title") ||
      !json_obj.contains("track_number") ||
      !json_obj.contains("duration") ||
      !json_obj.contains("copyright") ||
      !json_obj.contains("streamable")
    ) {
    Error("Invalid Json reply, track is missing one or more values.", json_obj);
    return;
  }

  QString song_id;
  if (json_obj["id"].isString()) {
    song_id = json_obj["id"].toString();
  }
  else {
    song_id = QString::number(json_obj["id"].toInt());
  }

  QString title = json_obj["title"].toString();
  int track = json_obj["track_number"].toInt();
  QString copyright = json_obj["copyright"].toString();
  qint64 duration = json_obj["duration"].toInt() * kNsecPerSec;
  //bool streamable = json_obj["streamable"].toBool();
  QString composer;
  QString performer;

  Artist song_artist = album_artist;
  Album song_album = album;
  if (json_obj.contains("album")) {

    QJsonValue value_album = json_obj["album"];
    if (!value_album.isObject()) {
      Error("Invalid Json reply, album is not an object.", value_album);
      return;
    }
    QJsonObject obj_album = value_album.toObject();

    if (obj_album.contains("id")) {
      if (obj_album["id"].isString()) {
        song_album.album_id = obj_album["id"].toString();
      }
      else {
        song_album.album_id = QString::number(obj_album["id"].toInt());
      }
    }

    if (obj_album.contains("title")) {
      song_album.album = obj_album["title"].toString();
    }

    if (obj_album.contains("artist")) {
      QJsonValue value_artist = obj_album["artist"];
      if (!value_artist.isObject()) {
        Error("Invalid Json reply, album artist is not a object.", value_artist);
        return;
      }
      QJsonObject obj_artist = value_artist.toObject();
      if (!obj_artist.contains("id") || !obj_artist.contains("name")) {
        Error("Invalid Json reply, album artist is missing id or name.", obj_artist);
        return;
      }
      if (obj_artist["id"].isString()) {
        song_artist.artist_id = obj_artist["id"].toString();
      }
      else {
        song_artist.artist_id = QString::number(obj_artist["id"].toInt());
      }
      song_artist.artist = obj_artist["name"].toString();
    }

    if (obj_album.contains("image")) {
      QJsonValue value_image = obj_album["image"];
      if (!value_image.isObject()) {
        Error("Invalid Json reply, album image is not a object.", value_image);
        return;
      }
      QJsonObject obj_image = value_image.toObject();
      if (!obj_image.contains("large")) {
        Error("Invalid Json reply, album image is missing large.", obj_image);
        return;
      }
      QString album_image = obj_image["large"].toString();
      if (!album_image.isEmpty()) {
        song_album.cover_url.setUrl(album_image);
      }
    }
  }

  if (json_obj.contains("composer")) {
    QJsonValue value_composer = json_obj["composer"];
    if (!value_composer.isObject()) {
      Error("Invalid Json reply, track composer is not a object.", value_composer);
      return;
    }
    QJsonObject obj_composer = value_composer.toObject();
    if (!obj_composer.contains("id") || !obj_composer.contains("name")) {
      Error("Invalid Json reply, track composer is missing id or name.", obj_composer);
      return;
    }
    composer = obj_composer["name"].toString();
  }

  if (json_obj.contains("performer")) {
    QJsonValue value_performer = json_obj["performer"];
    if (!value_performer.isObject()) {
      Error("Invalid Json reply, track performer is not a object.", value_performer);
      return;
    }
    QJsonObject obj_performer = value_performer.toObject();
    if (!obj_performer.contains("id") || !obj_performer.contains("name")) {
      Error("Invalid Json reply, track performer is missing id or name.", obj_performer);
      return;
    }
    performer = obj_performer["name"].toString();
  }

  //if (!streamable) {
  //Warn(QString("Song %1 %2 %3 is not streamable").arg(album_artist).arg(album).arg(title));
  //}

  QUrl url;
  url.setScheme(url_handler_->scheme());
  url.setPath(song_id);

  title.remove(Song::kTitleRemoveMisc);

  //qLog(Debug) << "id" << song_id << "track" << track << "title" << title << "album" << album << "album artist" << album_artist << cover_url << streamable << url;

  song.set_source(Song::Source_Qobuz);
  song.set_song_id(song_id);
  song.set_album_id(song_album.album_id);
  song.set_artist_id(song_artist.artist_id);
  song.set_album(song_album.album);
  song.set_artist(song_artist.artist);
  if (!album_artist.artist.isEmpty() && album_artist.artist != song_artist.artist) {
    song.set_albumartist(album_artist.artist);
  }
  song.set_title(title);
  song.set_track(track);
  song.set_url(url);
  song.set_length_nanosec(duration);
  song.set_art_automatic(song_album.cover_url);
  song.set_performer(performer);
  song.set_composer(composer);
  song.set_comment(copyright);
  song.set_directory_id(0);
  song.set_filetype(Song::FileType_Stream);
  song.set_filesize(0);
  song.set_mtime(0);
  song.set_ctime(0);
  song.set_valid(true);

}

void QobuzRequest::GetAlbumCoversCheck() {

  if (
      !finished_ &&
      service_->download_album_covers() &&
      IsQuery() &&
      artists_requests_queue_.isEmpty() &&
      albums_requests_queue_.isEmpty() &&
      songs_requests_queue_.isEmpty() &&
      artist_albums_requests_queue_.isEmpty() &&
      album_songs_requests_queue_.isEmpty() &&
      album_cover_requests_queue_.isEmpty() &&
      artist_albums_requests_pending_.isEmpty() &&
      album_songs_requests_pending_.isEmpty() &&
      album_covers_requests_sent_.isEmpty() &&
      artists_requests_active_ <= 0 &&
      albums_requests_active_ <= 0 &&
      songs_requests_active_ <= 0 &&
      artist_albums_requests_active_ <= 0 &&
      album_songs_requests_active_ <= 0 &&
      album_covers_requests_active_ <= 0
  ) {
    GetAlbumCovers();
  }

}

void QobuzRequest::GetAlbumCovers() {

  const SongList songs = songs_.values();
  for (const Song &song : songs) {
    AddAlbumCoverRequest(song);
  }

  if (album_covers_requests_total_ == 1) emit UpdateStatus(query_id_, tr("Receiving album cover for %1 album...").arg(album_covers_requests_total_));
  else emit UpdateStatus(query_id_, tr("Receiving album covers for %1 albums...").arg(album_covers_requests_total_));
  emit UpdateProgress(query_id_, 0);

  StartRequests();

}

void QobuzRequest::AddAlbumCoverRequest(const Song &song) {

  QUrl cover_url = song.art_automatic();
  if (!cover_url.isValid()) return;

  if (album_covers_requests_sent_.contains(cover_url)) {
    album_covers_requests_sent_.insert(cover_url, song.song_id());
    return;
  }

  AlbumCoverRequest request;
  request.url = cover_url;
  request.filename = app_->album_cover_loader()->CoverFilePath(song.source(), song.effective_albumartist(), song.effective_album(), song.album_id(), QString(), cover_url);
  if (request.filename.isEmpty()) return;

  album_covers_requests_sent_.insert(cover_url, song.song_id());
  ++album_covers_requests_total_;

  album_cover_requests_queue_.enqueue(request);

}

void QobuzRequest::FlushAlbumCoverRequests() {

  while (!album_cover_requests_queue_.isEmpty() && album_covers_requests_active_ < kMaxConcurrentAlbumCoverRequests) {

    AlbumCoverRequest request = album_cover_requests_queue_.dequeue();

    QNetworkRequest req(request.url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = network_->get(req);
    album_cover_replies_ << reply;
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, request]() { AlbumCoverReceived(reply, request.url, request.filename); });

    ++album_covers_requests_active_;

  }

}

void QobuzRequest::AlbumCoverReceived(QNetworkReply *reply, const QUrl &cover_url, const QString &filename) {

  if (album_cover_replies_.contains(reply)) {
    album_cover_replies_.removeAll(reply);
    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->deleteLater();
  }
  else {
    AlbumCoverFinishCheck();
    return;
  }

  --album_covers_requests_active_;
  ++album_covers_requests_received_;

  if (finished_) return;

  emit UpdateProgress(query_id_, GetProgress(album_covers_requests_received_, album_covers_requests_total_));

  if (!album_covers_requests_sent_.contains(cover_url)) {
    AlbumCoverFinishCheck();
    return;
  }

  if (reply->error() != QNetworkReply::NoError) {
    Error(QString("%1 (%2)").arg(reply->errorString()).arg(reply->error()));
    if (album_covers_requests_sent_.contains(cover_url)) album_covers_requests_sent_.remove(cover_url);
    AlbumCoverFinishCheck();
    return;
  }

  if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
    Error(QString("Received HTTP code %1 for %2.").arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()).arg(cover_url.toString()));
    if (album_covers_requests_sent_.contains(cover_url)) album_covers_requests_sent_.remove(cover_url);
    AlbumCoverFinishCheck();
    return;
  }

  QString mimetype = reply->header(QNetworkRequest::ContentTypeHeader).toString();
  if (mimetype.contains(';')) {
    mimetype = mimetype.left(mimetype.indexOf(';'));
  }
  if (!ImageUtils::SupportedImageMimeTypes().contains(mimetype, Qt::CaseInsensitive) && !ImageUtils::SupportedImageFormats().contains(mimetype, Qt::CaseInsensitive)) {
    Error(QString("Unsupported mimetype for image reader %1 for %2").arg(mimetype, cover_url.toString()));
    if (album_covers_requests_sent_.contains(cover_url)) album_covers_requests_sent_.remove(cover_url);
    AlbumCoverFinishCheck();
    return;
  }

  QByteArray data = reply->readAll();
  if (data.isEmpty()) {
    Error(QString("Received empty image data for %1").arg(cover_url.toString()));
    if (album_covers_requests_sent_.contains(cover_url)) album_covers_requests_sent_.remove(cover_url);
    AlbumCoverFinishCheck();
    return;
  }

  QList<QByteArray> format_list = ImageUtils::ImageFormatsForMimeType(mimetype.toUtf8());
  char *format = nullptr;
  if (!format_list.isEmpty()) {
    format = format_list.first().data();
  }

  QImage image;
  if (image.loadFromData(data, format)) {
    if (image.save(filename, format)) {
      while (album_covers_requests_sent_.contains(cover_url)) {
        const QString song_id = album_covers_requests_sent_.take(cover_url);
        if (songs_.contains(song_id)) {
          songs_[song_id].set_art_automatic(QUrl::fromLocalFile(filename));
        }
      }
    }
    else {
      Error(QString("Error saving image data to %1").arg(filename));
      if (album_covers_requests_sent_.contains(cover_url)) album_covers_requests_sent_.remove(cover_url);
    }
  }
  else {
    if (album_covers_requests_sent_.contains(cover_url)) album_covers_requests_sent_.remove(cover_url);
    Error(QString("Error decoding image data from %1").arg(cover_url.toString()));
  }

  AlbumCoverFinishCheck();

}

void QobuzRequest::AlbumCoverFinishCheck() {

  FinishCheck();

}

void QobuzRequest::FinishCheck() {

  if (
      !finished_ &&
      artists_requests_queue_.isEmpty() &&
      albums_requests_queue_.isEmpty() &&
      songs_requests_queue_.isEmpty() &&
      artist_albums_requests_queue_.isEmpty() &&
      album_songs_requests_queue_.isEmpty() &&
      album_cover_requests_queue_.isEmpty() &&
      artist_albums_requests_pending_.isEmpty() &&
      album_songs_requests_pending_.isEmpty() &&
      album_covers_requests_sent_.isEmpty() &&
      artists_requests_active_ <= 0 &&
      albums_requests_active_ <= 0 &&
      songs_requests_active_ <= 0 &&
      artist_albums_requests_active_ <= 0 &&
      album_songs_requests_active_ <= 0 &&
      album_covers_requests_active_ <= 0
  ) {
    if (timer_flush_requests_->isActive()) {
      timer_flush_requests_->stop();
    }
    finished_ = true;
    if (no_results_ && songs_.isEmpty()) {
      if (IsSearch())
        emit Results(query_id_, SongMap(), tr("No match."));
      else
        emit Results(query_id_, SongMap(), QString());
    }
    else {
      if (songs_.isEmpty() && errors_.isEmpty())
        emit Results(query_id_, songs_, tr("Unknown error"));
      else
        emit Results(query_id_, songs_, ErrorsToHTML(errors_));
    }
  }

}

int QobuzRequest::GetProgress(const int count, const int total) {

  return static_cast<int>((static_cast<float>(count) / static_cast<float>(total)) * 100.0F);

}

void QobuzRequest::Error(const QString &error, const QVariant &debug) {

  if (!error.isEmpty()) {
    errors_ << error;
    qLog(Error) << "Qobuz:" << error;
  }
  if (debug.isValid()) qLog(Debug) << debug;
  FinishCheck();

}

void QobuzRequest::Warn(const QString &error, const QVariant &debug) {

  qLog(Error) << "Qobuz:" << error;
  if (debug.isValid()) qLog(Debug) << debug;

}
