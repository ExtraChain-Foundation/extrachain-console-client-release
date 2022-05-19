#include "console/push_manager.h"

#include <QJsonObject>

#include "datastorage/index/actorindex.h"

PushManager::PushManager(ExtraChainNode *node, QObject *parent)
    : QObject(parent) {
    manager = new QNetworkAccessManager(this);
    connect(manager, &QNetworkAccessManager::finished, this, &PushManager::responseResolver);
}

void PushManager::pushNotification(QString actorId, Notification notification) {
    if (!QFile::exists("notification"))
        return;

    auto main = node->accountController()->mainActor();
    if (main.id() != node->actorIndex()->firstId())
        return;
    auto &key = main.key();

    QByteArray actorIdEncrypted = QByteArray::fromStdString(key.encryptSelf(actorId.toStdString()));
    DBConnector db("notification");
    db.open();
    auto res = actorId == "all" ? db.select("SELECT * FROM Notification")
                                : db.select("SELECT * FROM Notification WHERE actorId = ?;", "Notification",
                                            { { "actorId", actorIdEncrypted.toStdString() } });

    if (res.size() == 0) {
        qDebug() << "[Push] Actor with id" << actorId << "has not configured any push";
        return;
    }

    for (auto &&el : res) {
        QString token = QString::fromStdString(key.decryptSelf(el["token"]));
        QString os = QString::fromStdString(key.decryptSelf(el["os"]));

        if (os == "ios" || os == "android") {
            qDebug() << "[Push] New notification:" << actorId << notification;
            sendNotification(token, os, notification);
        }
    }
}

void PushManager::saveNotificationToken(QByteArray os, ActorId actorId, ActorId token) {
    static const std::string notificationTableCreation = //
        "CREATE TABLE IF NOT EXISTS Notification ("
        "token    BLOB PRIMARY KEY NOT NULL, "
        "actorId  BLOB             NOT NULL, "
        "os       BLOB             NOT NULL);";

    auto main = node->accountController()->mainActor();
    if (main.id() != node->actorIndex()->firstId())
        return;
    auto &key = main.key();

    const std::string &osActorId = actorId.toStdString();
    std::string apk = node->actorIndex()->getActor(actorId).key().publicKey();
    std::string osDecrypted = key.decrypt(os.toStdString(), apk);
    std::string osToken = key.decrypt(token.toStdString(), apk);

    if (osDecrypted != "ios" && osDecrypted != "android") {
        qDebug().noquote() << "[Push] Try save, but wrong os:" << QByteArray::fromStdString(osDecrypted);
        return;
    }

    DBConnector db("notification");
    db.open();
    db.createTable(notificationTableCreation);
    db.deleteRow("Notification", { { "token", osToken } });
    db.insert("Notification", { { "actorId", osActorId }, { "token", osToken }, { "os", osDecrypted } });

    qDebug() << "[Push] Saved" << QByteArray::fromStdString(osDecrypted)
             << QByteArray::fromStdString(osActorId) << QByteArray::fromStdString(osToken);
}

void PushManager::responseResolver(QNetworkReply *reply) {
    // qDebug() << "[Push] Result from url" << reply->url().toString();

    if (reply->error()) {
        qDebug() << "[Push] Error:" << reply->errorString();
        return;
    }

    QByteArray answer = reply->readAll();
    auto json = QJsonDocument::fromJson(answer);
    qDebug() << "[Push] Result:" << json;

    QString errorType = json["errorType"].toString();
    if (errorType == "None") {
        qDebug() << "[Push] Successfully sent for" << json["errorText"].toString();
    } else if (errorType == "BadDeviceToken" || errorType == "Unregistered") {
        QString token = json["errorText"].toString();
        qDebug() << "[Push] Remove token" << token;

        auto &main = node->accountController()->mainActor();
        auto &key = main.key();

        DBConnector db("notification");
        db.open();
        db.deleteRow("Notification", { { "token", key.encryptSelf(token.toStdString()) } });
    } else {
        qDebug() << "[Push] Error:" << errorType << json["errorText"].toString();
        // TODO: repeat request, ConnectionError
    }
}

void PushManager::chatMessage(QString sender, QString msgPath) {
    //    QString userId = msgPath.mid(DfsStruct::ROOT_FOOLDER_NAME_MID, 20);
    //    QString chatId = msgPath.mid(32, 64);

    //    std::string usersPath = CardManager::buildPathForFile(
    //        userId.toStdString(), chatId.toStdString() + "/users", DfsStruct::Type(104));
    //    DBConnector db(usersPath);
    //    db.open();
    //    auto res = db.select("SELECT * FROM Users");

    //    if (res.size() == 2) {
    //        QString users[2] = { QString::fromStdString(res[0]["userId"]),
    //                             QString::fromStdString(res[1]["userId"]) };

    //        pushNotification(
    //            users[0] == sender ? users[1] : users[0],
    //            Notification { .time = 100,
    //                           .type = Notification::NotifyType::ChatMsg,
    //                           .data = (users[0] == sender ? users[0] : users[1]).toLatin1() + " " });
    //    };
}

// void PushManager::fileAdded(QString path, QString original, DfsStruct::Type type, QString actorId) {
//     Q_UNUSED(original)

//    if (type == DfsStruct::Type::Post || type == DfsStruct::Type::Event) {
//        if (path.contains("."))
//            return;

//        Notification notify { .time = 100,
//                              .type = type == DfsStruct::Type::Post ? Notification::NewPost
//                                                                    : Notification::NewEvent,
//                              .data = actorId.toLatin1() + " " };
//        pushNotification("all", notify);
//    }
//}

void PushManager::sendNotification(const QString &token, const QString &os,
                                   const Notification &notification) {
    QJsonObject json;
    json["os"] = os;
    json["token"] = token;
    json["message"] = notificationToMessage(notification);

    QJsonObject data;
    data["notifyType"] = notification.type;
    data["data"] = QString(notification.data);
    json["data"] = data;

    QString jsonStr = QJsonDocument(json).toJson();
    qDebug() << "[Push] Send" << json;
    manager->get(QNetworkRequest(QUrl(pushServerUrl + jsonStr)));
}

QString PushManager::notificationToMessage(const Notification &notification) {
    QString message;
    QByteArray userId;

    switch (notification.type) {
    case (Notification::NotifyType::TxToUser):
        message = "Transaction to *" + notification.data.right(5) + " completed";
        userId = "";
        break;
    case (Notification::NotifyType::TxToMe):
        message = "New transaction from *" + notification.data.right(5);
        userId = "";
        break;
    case (Notification::NotifyType::ChatMsg): {
        QByteArray chatId = notification.data.split(' ').at(0);
        message = "New message from ";
        userId = chatId;
        break;
    }
    case (Notification::NotifyType::ChatInvite): {
        QByteArray user = notification.data.split(' ').at(0);
        message = "New chat from ";
        userId = user;
        break;
    }
    case (Notification::NotifyType::NewPost): {
        QByteArray user = notification.data.split(' ').at(0);
        message = "New post from ";
        userId = user;
        break;
    }
    case (Notification::NotifyType::NewEvent): {
        QByteArray user = notification.data.split(' ').at(0);
        message = "New event from ";
        userId = user;
        break;
    }
    case (Notification::NotifyType::NewFollower):
        message = "New follower ";
        userId = notification.data;
        break;
    }

    if (userId.isEmpty())
        return message;

    return "";

    //    if (actorIndex == nullptr || !actorIndex)
    //    {
    //        qFatal("[Push] No actor index access");
    //    }
    /*
    QByteArrayList profile = node->actorIndex()->getProfile(userId);
    if (profile.isEmpty())
        return message;
    else {
        QString name = profile.at(3);
        QString secName = profile.at(4);
        return message + name + " " + secName;
    }
    */
}
