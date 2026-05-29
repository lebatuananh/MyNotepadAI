/*
 * This file is part of Notepad Next.
 * Copyright 2026 NotepadAI contributors
 *
 * Notepad Next is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Notepad Next is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Notepad Next.  If not, see <https://www.gnu.org/licenses/>.
 */

// notepadai-editor: helper executable invoked by git as GIT_SEQUENCE_EDITOR
// or GIT_EDITOR. Connects to the main NotepadAI process via TCP loopback,
// sends the file path, and waits for a reply (0 = success, 1 = cancel).

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QTcpSocket>
#include <QTimer>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("notepadai-editor"));

    QCommandLineParser parser;
    parser.addOption({QStringLiteral("port"), QStringLiteral("TCP port"), QStringLiteral("port")});
    parser.addOption({QStringLiteral("token"), QStringLiteral("Auth token"), QStringLiteral("token")});
    parser.addPositionalArgument(QStringLiteral("file"), QStringLiteral("File to edit"));
    parser.process(app);

    int port = parser.value(QStringLiteral("port")).toInt();
    QString token = parser.value(QStringLiteral("token"));
    QStringList positional = parser.positionalArguments();

    if (port <= 0 || token.isEmpty() || positional.isEmpty())
        return 1;

    const QString &filePath = positional.first();

    QTcpSocket socket;
    socket.connectToHost(QStringLiteral("127.0.0.1"), static_cast<quint16>(port));

    if (!socket.waitForConnected(5000))
        return 1;

    QByteArray payload = token.toUtf8() + '\n' + filePath.toUtf8() + '\n';
    socket.write(payload);
    socket.flush();

    // Wait for reply with a 10-minute timeout (user may take time in the dialog)
    QTimer timeout;
    timeout.setSingleShot(true);
    timeout.setInterval(600000);

    int exitCode = 1;
    bool done = false;

    QObject::connect(&socket, &QTcpSocket::readyRead, &app, [&]() {
        QByteArray reply = socket.readLine().trimmed();
        exitCode = reply.toInt();
        done = true;
        app.quit();
    });

    QObject::connect(&socket, &QTcpSocket::disconnected, &app, [&]() {
        if (!done) {
            exitCode = 1;
            app.quit();
        }
    });

    QObject::connect(&timeout, &QTimer::timeout, &app, [&]() {
        exitCode = 1;
        app.quit();
    });

    timeout.start();
    app.exec();

    return exitCode;
}
