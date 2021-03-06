#include <QtGlobal>

#ifdef Q_OS_WIN
  #ifdef USE_SDL_2
  #include <SDL2/SDL.h>
  #else
  #include <SDL/SDL.h>
  #endif
  #undef main
#endif

#include <QApplication>
#include <QMainWindow>
#include <QHash>
#include <QHashIterator>
#include <QDir>
#include <QDebug>
#include <QTranslator>
#include <QLibraryInfo>
#include <QSystemTrayIcon>
#include <QTextStream>
#include <QLocalSocket>

#ifdef Q_OS_WIN
#include <QStyle>
#include <QStyleFactory>
#endif

#include "joystick.h"
#include "joybuttonslot.h"
#include "inputdaemon.h"
#include "common.h"
#include "advancebuttondialog.h"
#include "commandlineutility.h"
#include "mainwindow.h"
#include "inputdevice.h"

#ifndef Q_OS_WIN
#include <signal.h>
#endif

#ifndef Q_OS_WIN
static void termSignalHandler(int signal)
{
    Q_UNUSED(signal);

    qApp->exit(0);
}
#endif


int main(int argc, char *argv[])
{
    qRegisterMetaType<JoyButtonSlot*>();
    qRegisterMetaType<AdvanceButtonDialog*>();
    //qRegisterMetaType<Joystick*>();
    qRegisterMetaType<InputDevice*>();

    // If running Win version, check if an explicit style
    // was defined on the command-line. If so, make a note
    // of it.
#ifdef Q_OS_WIN
    bool styleChangeFound = false;
    for (int i=0; i < argc && !styleChangeFound; i++)
    {
        char *tempchrstr = argv[i];
        QString temp = QString::fromUtf8(tempchrstr);
        if (temp == "-style")
        {
            styleChangeFound = true;
        }
    }
#endif

    QApplication a(argc, argv);
    //QString defaultStyleName = qApp->style()->objectName();

    // If running Win version and no explicit style was
    // defined, use the style Fusion by default. I find the
    // windowsvista style a tad ugly
#ifdef Q_OS_WIN
    if (!styleChangeFound)
    {
        qApp->setStyle(QStyleFactory::create("Fusion"));
    }
#endif

    CommandLineUtility cmdutility;
    QStringList cmdarguments = a.arguments();
    cmdutility.parseArguments(cmdarguments);

    QTranslator qtTranslator;
    qtTranslator.load("qt_" + QLocale::system().name(), QLibraryInfo::location(QLibraryInfo::TranslationsPath));
    a.installTranslator(&qtTranslator);

    QTranslator myappTranslator;
#if defined(Q_OS_UNIX)
    myappTranslator.load("antimicro_" + QLocale::system().name(), QApplication::applicationDirPath().append("/../share/antimicro/translations"));
#elif defined(Q_OS_WIN)
    myappTranslator.load("antimicro_" + QLocale::system().name(), QApplication::applicationDirPath().append("\\share\\antimicro\\translations"));
#endif
    a.installTranslator(&myappTranslator);

    if (cmdutility.hasError())
    {
        return 1;
    }
    else if (cmdutility.isHelpRequested())
    {
        cmdutility.printHelp();
        return 0;
    }
    else if (cmdutility.isVersionRequested())
    {
        cmdutility.printVersionString();
        return 0;
    }

    Q_INIT_RESOURCE(resources);
    a.setQuitOnLastWindowClosed(false);

    QDir configDir (PadderCommon::configPath);
    if (!configDir.exists())
    {
        configDir.mkpath(PadderCommon::configPath);
    }

#ifdef USE_SDL_2
    QHash<SDL_JoystickID, InputDevice*> *joysticks = new QHash<SDL_JoystickID, InputDevice*>();
#else
    QHash<int, InputDevice*> *joysticks = new QHash<int, InputDevice*>();
#endif

    // Cross-platform way of performing IPC. Currently,
    // only establish a connection and then disconnect.
    // In the future, there might be a reason to actually send
    // messages to the QLocalServer.
    QLocalSocket socket;
    socket.connectToServer(PadderCommon::localSocketKey);
    socket.waitForConnected(1000);
    if (socket.state() == QLocalSocket::ConnectedState)
    {
        // An instance of this program is already running.
        // Save app config and exit.
        InputDaemon *joypad_worker = new InputDaemon(joysticks, false);
        MainWindow w(joysticks, &cmdutility, false);

        if (!cmdutility.hasError() && cmdutility.hasProfile())
        {
            w.saveAppConfig();
        }

        joypad_worker->quit();
        w.removeJoyTabs();

        socket.disconnectFromServer();

#ifdef USE_SDL_2
        QHashIterator<SDL_JoystickID, InputDevice*> iter(*joysticks);
#else
        QHashIterator<int, InputDevice*> iter(*joysticks);
#endif

        while (iter.hasNext())
        {
            InputDevice *joystick = iter.next().value();
            if (joystick)
            {
                delete joystick;
                joystick = 0;
            }
        }

        joysticks->clear();
        delete joysticks;
        joysticks = 0;

        delete joypad_worker;
        joypad_worker = 0;

        return 0;
    }

    InputDaemon *joypad_worker = new InputDaemon (joysticks);
    MainWindow w(joysticks, &cmdutility);
    w.startLocalServer();

#ifndef Q_OS_WIN
    struct sigaction termaction;
    termaction.sa_handler = &termSignalHandler;
    sigemptyset(&termaction.sa_mask);
    termaction.sa_flags = 0;

    sigaction(SIGTERM, &termaction, 0);
#endif

#ifdef USE_SDL_2
    QObject::connect(joypad_worker, SIGNAL(joysticksRefreshed(QHash<SDL_JoystickID, InputDevice*>*)), &w, SLOT(fillButtons(QHash<SDL_JoystickID, InputDevice*>*)));
#else
    QObject::connect(joypad_worker, SIGNAL(joysticksRefreshed(QHash<int, InputDevice*>*)), &w, SLOT(fillButtons(QHash<int, InputDevice*>*)));
#endif
    QObject::connect(&w, SIGNAL(joystickRefreshRequested()), joypad_worker, SLOT(refresh()));
    QObject::connect(joypad_worker, SIGNAL(joystickRefreshed(InputDevice*)), &w, SLOT(fillButtons(InputDevice*)));
    QObject::connect(&a, SIGNAL(lastWindowClosed()), &a, SLOT(quit()));
    QObject::connect(&a, SIGNAL(aboutToQuit()), &w, SLOT(saveAppConfig()));
    QObject::connect(&a, SIGNAL(aboutToQuit()), &w, SLOT(removeJoyTabs()));
    QObject::connect(&a, SIGNAL(aboutToQuit()), joypad_worker, SLOT(quit()));
#ifdef USE_SDL_2
    QObject::connect(&w, SIGNAL(mappingUpdated(QString,InputDevice*)), joypad_worker, SLOT(refreshMapping(QString,InputDevice*)));
    QObject::connect(joypad_worker, SIGNAL(deviceUpdated(int,InputDevice*)), &w, SLOT(testMappingUpdateNow(int,InputDevice*)));
    QObject::connect(joypad_worker, SIGNAL(deviceRemoved(SDL_JoystickID)), &w, SLOT(removeJoyTab(SDL_JoystickID)));
    QObject::connect(joypad_worker, SIGNAL(deviceAdded(InputDevice*)), &w, SLOT(addJoyTab(InputDevice*)));
#endif

    /*if (!cmdutility.isHiddenRequested() && (!cmdutility.isLaunchInTrayEnabled() || !QSystemTrayIcon::isSystemTrayAvailable()))
    {
        w.show();
    }*/

    int app_result = a.exec();

#ifdef USE_SDL_2
    QHashIterator<SDL_JoystickID, InputDevice*> iter(*joysticks);
#else
    QHashIterator<int, InputDevice*> iter(*joysticks);
#endif

    while (iter.hasNext())
    {
        InputDevice *joystick = iter.next().value();
        if (joystick)
        {
            delete joystick;
            joystick = 0;
        }
    }

    joysticks->clear();
    delete joysticks;
    joysticks = 0;

    delete joypad_worker;
    joypad_worker = 0;

    return app_result;
}
