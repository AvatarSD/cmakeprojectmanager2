/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 as published by the Free Software
** Foundation with exceptions as appearing in the file LICENSE.GPL3-EXCEPT
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
****************************************************************************/

#include "builddirmanager.h"
#include "cmakekitinformation.h"
#include "cmakeprojectmanager.h"
#include "cmaketool.h"

#include <coreplugin/messagemanager.h>
#include <projectexplorer/kit.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/projectnodes.h>
#include <projectexplorer/taskhub.h>

#include <utils/algorithm.h>
#include <utils/qtcassert.h>
#include <utils/synchronousprocess.h>

#include <QDateTime>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QRegularExpression>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>

// --------------------------------------------------------------------
// Helper:
// --------------------------------------------------------------------

namespace CMakeProjectManager {
namespace Internal {

static QStringList toArguments(const CMakeConfig &config) {
    return Utils::transform(config, [](const CMakeConfigItem &i) -> QString {
                                 QString a = QString::fromLatin1("-D");
                                 a.append(QString::fromUtf8(i.key));
                                 switch (i.type) {
                                 case CMakeConfigItem::FILEPATH:
                                     a.append(QLatin1String(":FILEPATH="));
                                     break;
                                 case CMakeConfigItem::PATH:
                                     a.append(QLatin1String(":PATH="));
                                     break;
                                 case CMakeConfigItem::BOOL:
                                     a.append(QLatin1String(":BOOL="));
                                     break;
                                 case CMakeConfigItem::STRING:
                                     a.append(QLatin1String(":STRING="));
                                     break;
                                 case CMakeConfigItem::INTERNAL:
                                     a.append(QLatin1String(":INTERNAL="));
                                     break;
                                 }
                                 a.append(QString::fromUtf8(i.value));

                                 return a;
                             });
}

// --------------------------------------------------------------------
// BuildDirManager:
// --------------------------------------------------------------------

BuildDirManager::BuildDirManager(const Utils::FileName &sourceDir, const ProjectExplorer::Kit *k,
                                 const CMakeConfig &inputConfig, const Utils::Environment &env,
                                 const Utils::FileName &buildDir) :
    m_sourceDir(sourceDir),
    m_buildDir(buildDir),
    m_kit(k),
    m_environment(env),
    m_inputConfig(inputConfig),
    m_watcher(new QFileSystemWatcher(this))
{
    QTC_CHECK(!sourceDir.isEmpty());
    m_projectName = m_sourceDir.fileName();
    if (m_buildDir.isEmpty()) {
        m_tempDir = new QTemporaryDir(QLatin1String("cmake-tmp-XXXXXX"));
        m_buildDir = Utils::FileName::fromString(m_tempDir->path());
    }
    QTC_CHECK(!m_buildDir.isEmpty());
    QTC_CHECK(k);

    connect(m_watcher, &QFileSystemWatcher::fileChanged, this, [this]() {
        if (!isBusy())
            forceReparse();
    });

    QTimer::singleShot(0, this, &BuildDirManager::parse);
}

BuildDirManager::~BuildDirManager()
{
    delete m_tempDir;
}

bool BuildDirManager::isBusy() const
{
    if (m_cmakeProcess)
        return m_cmakeProcess->state() != QProcess::NotRunning;
    return false;
}

void BuildDirManager::forceReparse()
{
    CMakeTool *tool = CMakeKitInformation::cmakeTool(m_kit);
    const QString generator = CMakeGeneratorKitInformation::generator(m_kit);

    QTC_ASSERT(tool, return);
    QTC_ASSERT(!generator.isEmpty(), return);

    startCMake(tool, generator, m_inputConfig);
}

void BuildDirManager::parse()
{
    CMakeTool *tool = CMakeKitInformation::cmakeTool(m_kit);
    const QString generator = CMakeGeneratorKitInformation::generator(m_kit);

    QTC_ASSERT(tool, return);
    QTC_ASSERT(!generator.isEmpty(), return);

    // Pop up a dialog asking the user to rerun cmake
    QString cbpFile = CMakeManager::findCbpFile(QDir(m_buildDir.toString()));
    QFileInfo cbpFileFi(cbpFile);

    if (!cbpFileFi.exists()) {
        // Initial create:
        startCMake(tool, generator, m_inputConfig);
        return;
    }

    const bool mustUpdate
            = Utils::anyOf(m_watchedFiles, [&cbpFileFi](const Utils::FileName &f) {
                  return f.toFileInfo().lastModified() > cbpFileFi.lastModified();
              });
    if (mustUpdate) {
        startCMake(tool, generator, CMakeConfig());
    } else {
        extractData();
        emit dataAvailable();
    }
}

bool BuildDirManager::isProjectFile(const Utils::FileName &fileName) const
{
    return m_watchedFiles.contains(fileName);
}

QString BuildDirManager::projectName() const
{
    return m_projectName;
}

QList<CMakeBuildTarget> BuildDirManager::buildTargets() const
{
    return m_buildTargets;
}

QList<ProjectExplorer::FileNode *> BuildDirManager::files() const
{
    return m_files;
}

void BuildDirManager::extractData()
{
    const Utils::FileName topCMake
            = Utils::FileName::fromString(m_sourceDir.toString() + QLatin1String("/CMakeLists.txt"));

    m_projectName = m_sourceDir.fileName();
    m_buildTargets.clear();
    m_watchedFiles.clear();
    m_files.clear();
    m_files.append(new ProjectExplorer::FileNode(topCMake, ProjectExplorer::ProjectFileType, false));
    m_watchedFiles.insert(topCMake);

    foreach (const QString &file, m_watcher->files())
        m_watcher->removePath(file);

    // Find cbp file
    QString cbpFile = CMakeManager::findCbpFile(m_buildDir.toString());
    if (cbpFile.isEmpty())
        return;

    m_watcher->addPath(cbpFile);

    // setFolderName
    CMakeCbpParser cbpparser;
    // Parsing
    if (!cbpparser.parseCbpFile(m_kit, cbpFile, m_sourceDir.toString()))
        return;

    m_projectName = cbpparser.projectName();

    m_files = cbpparser.fileList();
    QSet<Utils::FileName> projectFiles;
    if (cbpparser.hasCMakeFiles()) {
        m_files.append(cbpparser.cmakeFileList());
        foreach (const ProjectExplorer::FileNode *node, cbpparser.cmakeFileList())
            projectFiles.insert(node->filePath());
    } else {
        m_files.append(new ProjectExplorer::FileNode(topCMake, ProjectExplorer::ProjectFileType, false));
        projectFiles.insert(topCMake);
    }

    m_watchedFiles = projectFiles;
    foreach (const Utils::FileName &f, m_watchedFiles)
        m_watcher->addPath(f.toString());

    m_buildTargets = cbpparser.buildTargets();
}

void BuildDirManager::startCMake(CMakeTool *tool, const QString &generator,
                                 const CMakeConfig &config)
{
    QTC_ASSERT(tool && tool->isValid(), return);
    QTC_ASSERT(!m_cmakeProcess, return);

    // Make sure m_buildDir exists:
    const QString buildDirStr = m_buildDir.toString();
    QDir bDir = QDir(buildDirStr);
    bDir.mkpath(buildDirStr);

    // Always use the sourceDir: If we are triggered because the build directory is getting deleted
    // then we are racing against CMakeCache.txt also getting deleted.
    const QString srcDir = m_sourceDir.toString();

    m_cmakeProcess = new Utils::QtcProcess(this);
    m_cmakeProcess->setProcessChannelMode(QProcess::MergedChannels);
    m_cmakeProcess->setWorkingDirectory(buildDirStr);
    m_cmakeProcess->setEnvironment(m_environment);

    connect(m_cmakeProcess, &QProcess::readyReadStandardOutput,
            this, &BuildDirManager::processCMakeOutput);
    connect(m_cmakeProcess, static_cast<void(QProcess::*)(int,  QProcess::ExitStatus)>(&QProcess::finished),
            this, &BuildDirManager::cmakeFinished);

    QString args;
    Utils::QtcProcess::addArg(&args, srcDir);
    if (!generator.isEmpty())
        Utils::QtcProcess::addArg(&args, QString::fromLatin1("-G%1").arg(generator));
    Utils::QtcProcess::addArgs(&args, toArguments(config));

    // Clear task cache:
    ProjectExplorer::TaskHub::clearTasks(ProjectExplorer::Constants::TASK_CATEGORY_BUILDSYSTEM);
    m_toReport.clear();

    m_cmakeProcess->setCommand(tool->cmakeExecutable().toString(), args);
    m_cmakeProcess->start();
    emit parsingStarted();
}

void BuildDirManager::cmakeFinished(int code, QProcess::ExitStatus status)
{
    QTC_ASSERT(m_cmakeProcess, return);

    // process rest of the output:
    while (m_cmakeProcess->canReadLine())
        processOutputLine(Utils::SynchronousProcess::normalizeNewlines(QString::fromLocal8Bit(m_cmakeProcess->readLine())));
    QString rest = Utils::SynchronousProcess::normalizeNewlines(QString::fromLocal8Bit(m_cmakeProcess->readAllStandardOutput()));
    if (!rest.isEmpty())
        processOutputLine(rest);

    QTC_CHECK(m_cmakeProcess->readAllStandardOutput().isEmpty());

    if (!m_toReport.description.isEmpty())
        ProjectExplorer::TaskHub::addTask(m_toReport);
    m_toReport.clear();

    m_cmakeProcess->deleteLater();
    m_cmakeProcess = nullptr;

    extractData(); // try even if cmake failed...

    if (status != QProcess::NormalExit)
        Core::MessageManager::write(tr("*** cmake process crashed!"));
    else if (code != 0)
        Core::MessageManager::write(tr("*** cmake process exited with exit code %s.").arg(code));
    emit dataAvailable();
}

void BuildDirManager::processCMakeOutput()
{
    QTC_ASSERT(m_cmakeProcess, return);
    while (m_cmakeProcess->canReadLine())
        processOutputLine(QString::fromLocal8Bit(m_cmakeProcess->readLine()));
}

void BuildDirManager::processOutputLine(const QString &l)
{
    QString line = Utils::SynchronousProcess::normalizeNewlines(l);
    while (line.endsWith(QLatin1Char('\n')))
        line.chop(1);
    Core::MessageManager::write(line);

    // Check for errors:
    if (m_toReport.type == ProjectExplorer::Task::Unknown) {
        m_toReport.category = ProjectExplorer::Constants::TASK_CATEGORY_BUILDSYSTEM;
    } else {
        if (line.startsWith(QStringLiteral("  ")) || line.isEmpty()) {
            m_toReport.description.append(QStringLiteral("\n") + line);
        } else {
            ProjectExplorer::TaskHub::addTask(m_toReport);
            m_toReport.clear();
        }
    }
}

} // namespace Internal
} // namespace CMakeProjectManager