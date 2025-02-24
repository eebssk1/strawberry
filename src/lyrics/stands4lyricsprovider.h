/*
 * Strawberry Music Player
 * Copyright 2023, Jonas Kvinge <jonas@jkvinge.net>
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

#ifndef STANDS4LYRICSPROVIDER_H
#define STANDS4LYRICSPROVIDER_H

#include <QtGlobal>
#include <QObject>
#include <QList>
#include <QVariant>
#include <QString>

#include "lyricsprovider.h"
#include "lyricsfetcher.h"

class QNetworkReply;
class NetworkAccessManager;

class Stands4LyricsProvider : public LyricsProvider {
  Q_OBJECT

 public:
  explicit Stands4LyricsProvider(NetworkAccessManager *network, QObject *parent = nullptr);
  ~Stands4LyricsProvider() override;

  bool StartSearch(const QString &artist, const QString &album, const QString &title, int id) override;
  void CancelSearch(const int id) override;

 private:
  void Error(const QString &error, const QVariant &debug = QVariant()) override;
  static QString StringFixup(QString string);

 private slots:
  void HandleSearchReply(QNetworkReply *reply, const int id, const QString &artist, const QString &title);

 private:
  static const char *kUrl;
  QList<QNetworkReply*> replies_;
};

#endif  // STANDS4LYRICSPROVIDER_H
