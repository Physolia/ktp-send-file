/*
 * Main Send File Transfer Window
 *
 * Copyright (C) 2011 David Edmundson <kde@davidedmundson.co.uk>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "mainwindow.h"
#include "ui_mainwindow.h"

#include <KFileItem>
#include <KFileItemList>
#include <KIO/PreviewJob>
#include <KApplication>
#include <KCmdLineArgs>
#include <KMimeType>
#include <KDebug>
#include <KMessageBox>
#include <KPixmapSequence>
#include <KPixmapSequenceOverlayPainter>
#include <KDE/KIO/PreviewJob>

#include <QAbstractItemDelegate>
#include <QPainter>
#include <QRect>
#include <QStyle>
#include <QDebug>
#include <QAbstractButton>

#include <TelepathyQt/AccountManager>
#include <TelepathyQt/PendingChannelRequest>
#include <TelepathyQt/PendingReady>

#include "flat-model-proxy.h"

#include <KTp/Models/accounts-model.h>
#include <KTp/Models/accounts-filter-model.h>
#include <KTp/Models/contact-model-item.h>

//FIXME, copy and paste the approver code for loading this from a config file into this, the contact list and the chat handler.
#define PREFERRED_FILETRANSFER_HANDLER "org.freedesktop.Telepathy.Client.KDE.FileTransfer"


class ContactGridDelegate : public QAbstractItemDelegate {
public:
    ContactGridDelegate(QObject *parent);
    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const;
};

ContactGridDelegate::ContactGridDelegate(QObject *parent)
    : QAbstractItemDelegate(parent)
{

}

void ContactGridDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QStyle *style = QApplication::style();
    int textHeight = option.fontMetrics.height()*2;

    style->drawPrimitive(QStyle::PE_PanelItemViewItem, &option, painter);

    QRect avatarRect = option.rect.adjusted(0,0,0,-textHeight);
    QRect textRect = option.rect.adjusted(0,option.rect.height()-textHeight,0,-3);

    QPixmap avatar = index.data(Qt::DecorationRole).value<QPixmap>();
    if (avatar.isNull()) {
        avatar = KIcon("im-user-online").pixmap(QSize(70,70));
    }

    //resize larger avatars
    if (avatar.width() > 80 || avatar.height()> 80) {
        avatar = avatar.scaled(QSize(80,80), Qt::KeepAspectRatio);
        //draw leaving paddings on smaller (or non square) avatars
    }
    style->drawItemPixmap(painter, avatarRect, Qt::AlignCenter, avatar);


    QTextOption textOption;
    textOption.setAlignment(Qt::AlignCenter);
    textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    painter->drawText(textRect, index.data().toString(), textOption);

}

QSize ContactGridDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(index);
    int textHeight = option.fontMetrics.height()*2;
    return QSize(84, 80 + textHeight + 3);
}


MainWindow::MainWindow(const KUrl &url, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::MainWindow),
    m_accountsModel(0)
{
    Tp::registerTypes();

    ui->setupUi(this);
    setWindowTitle(i18n("Send file - %1", url.fileName()));

    kDebug() << KApplication::arguments();

    ui->fileNameLabel->setText(url.fileName());
    m_busyOverlay = new KPixmapSequenceOverlayPainter(this);
    m_busyOverlay->setSequence(KPixmapSequence("process-working", 22));
    m_busyOverlay->setWidget(ui->filePreview);
    m_busyOverlay->start();

    KFileItem file(KFileItem::Unknown, KFileItem::Unknown, url);
    QStringList availablePlugins = KIO::PreviewJob::availablePlugins();
    KIO::PreviewJob* job = KIO::filePreview(KFileItemList() << file, QSize(280, 280), &availablePlugins);
    job->setOverlayIconAlpha(0);
    job->setScaleType(KIO::PreviewJob::Unscaled);
    connect(job, SIGNAL(gotPreview(KFileItem,QPixmap)),
            this, SLOT(onPreviewLoaded(KFileItem,QPixmap)));
    connect(job, SIGNAL(failed(KFileItem)),
            this, SLOT(onPreviewFailed(KFileItem)));


    Tp::AccountFactoryPtr  accountFactory = Tp::AccountFactory::create(QDBusConnection::sessionBus(),
                                                                       Tp::Features() << Tp::Account::FeatureCore
                                                                       << Tp::Account::FeatureAvatar
                                                                       << Tp::Account::FeatureProtocolInfo
                                                                       << Tp::Account::FeatureProfile);

    Tp::ConnectionFactoryPtr connectionFactory = Tp::ConnectionFactory::create(QDBusConnection::sessionBus(),
                                                                               Tp::Features() << Tp::Connection::FeatureCore
                                                                               << Tp::Connection::FeatureRosterGroups
                                                                               << Tp::Connection::FeatureRoster
                                                                               << Tp::Connection::FeatureSelfContact);

    Tp::ContactFactoryPtr contactFactory = Tp::ContactFactory::create(Tp::Features()  << Tp::Contact::FeatureAlias
                                                                      << Tp::Contact::FeatureAvatarData
                                                                      << Tp::Contact::FeatureSimplePresence
                                                                      << Tp::Contact::FeatureCapabilities);

    Tp::ChannelFactoryPtr channelFactory = Tp::ChannelFactory::create(QDBusConnection::sessionBus());

    m_accountManager = Tp::AccountManager::create(QDBusConnection::sessionBus(),
                                                  accountFactory,
                                                  connectionFactory,
                                                  channelFactory,
                                                  contactFactory);


    connect(m_accountManager->becomeReady(), SIGNAL(finished(Tp::PendingOperation*)), SLOT(onAccountManagerReady()));

    connect(ui->buttonBox, SIGNAL(accepted()), SLOT(onDialogAccepted()));
    connect(ui->buttonBox, SIGNAL(rejected()), SLOT(close()));
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::onAccountManagerReady()
{
    m_accountsModel = new AccountsModel(m_accountManager, this);
    AccountsFilterModel *filterModel = new AccountsFilterModel(this);
    filterModel->setSourceModel(m_accountsModel);
    filterModel->setShowOfflineUsers(false);
    filterModel->setFilterByFileTransferCapability(true);

    connect(ui->filterBar, SIGNAL(textChanged(QString)),
            filterModel, SLOT(setFilterString(QString)));

    FlatModelProxy *flatProxyModel = new FlatModelProxy(filterModel);

    ui->listView->setModel(flatProxyModel);
    ui->listView->setItemDelegate(new ContactGridDelegate(this));
}

void MainWindow::onDialogAccepted()
{
    // don't do anytghing if no contact has been selected
    if (!ui->listView->currentIndex().isValid()) {
        // show message box?
        return;
    }

    ContactModelItem *contactModelItem = ui->listView->currentIndex().data(AccountsModel::ItemRole).value<ContactModelItem*>();
    Tp::ContactPtr contact = contactModelItem->contact();
    Tp::AccountPtr sendingAccount = m_accountsModel->accountForContactItem(contactModelItem);

    if (sendingAccount.isNull()) {
        kDebug() << "sending account: NULL";
    } else {
        kDebug() << "Account is: " << sendingAccount->displayName();
        kDebug() << "sending to: " << contact->alias();
    }

    // start sending file
    QString filePath (KCmdLineArgs::parsedArgs()->arg(0));
    qDebug() << "FILE TO SEND: " << filePath;

    Tp::FileTransferChannelCreationProperties fileTransferProperties(filePath, KMimeType::findByFileContent(filePath)->name());

    Tp::PendingChannelRequest* channelRequest = sendingAccount->createFileTransfer(contact,
                                                                                   fileTransferProperties,
                                                                                   QDateTime::currentDateTime(),
                                                                                   PREFERRED_FILETRANSFER_HANDLER);

    connect(channelRequest, SIGNAL(finished(Tp::PendingOperation*)), SLOT(slotFileTransferFinished(Tp::PendingOperation*)));

    //disable the buttons
    foreach(QAbstractButton* button, ui->buttonBox->buttons()) {
        button->setEnabled(false);
    }
}

void MainWindow::slotFileTransferFinished(Tp::PendingOperation* op)
{
    if (op->isError()) {
        //FIXME map to human readable strings.
        QString errorMsg(op->errorName() + ": " + op->errorMessage());
        kDebug() << "ERROR!: " << errorMsg;
        KMessageBox::error(this, i18n("Failed to send file"), i18n("File Transfer Failed"));
        close();
    } else {
        kDebug() << "Transfer started";
        // now I can close the dialog
        close();
    }
}

void MainWindow::onPreviewLoaded(const KFileItem& item, const QPixmap& preview)
{
    Q_UNUSED(item);
    ui->filePreview->setPixmap(preview);
    m_busyOverlay->stop();
}

void MainWindow::onPreviewFailed(const KFileItem& item)
{
    kWarning() << "Loading thumb failed" << item.name();
    ui->filePreview->setPixmap(KIconLoader::global()->loadIcon(item.iconName(), KIconLoader::Desktop, 128));
    m_busyOverlay->stop();
}
