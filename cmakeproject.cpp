/****************************************************************************
**
** Copyright (C) 2014 Digia Plc and/or its subsidiary(-ies).
** Contact: http://www.qt-project.org/legal
**
** This file is part of Qt Creator.
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and Digia.  For licensing terms and
** conditions see http://qt.digia.com/licensing.  For further information
** use the contact form at http://qt.digia.com/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL included in the
** packaging of this file.  Please review the following information to
** ensure the GNU Lesser General Public License version 2.1 requirements
** will be met: http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Digia gives you certain additional
** rights.  These rights are described in the Digia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
****************************************************************************/

#include "cmakeproject.h"

#include "cmakebuildconfiguration.h"
#include "cmakeprojectconstants.h"
#include "cmakeprojectnodes.h"
#include "cmakerunconfiguration.h"
#include "makestep.h"
#include "cmakeopenprojectwizard.h"
#include "generatorinfo.h"

#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/projectexplorer.h>
#include <projectexplorer/headerpath.h>
#include <projectexplorer/buildsteplist.h>
#include <projectexplorer/buildmanager.h>
#include <projectexplorer/buildtargetinfo.h>
#include <projectexplorer/kitinformation.h>
#include <projectexplorer/kitmanager.h>
#include <projectexplorer/toolchain.h>
#include <projectexplorer/target.h>
#include <projectexplorer/deployconfiguration.h>
#include <projectexplorer/deploymentdata.h>
#include <projectexplorer/projectmacroexpander.h>
#include <qtsupport/customexecutablerunconfiguration.h>
#include <qtsupport/baseqtversion.h>
#include <qtsupport/qtkitinformation.h>
#include <qtsupport/uicodemodelsupport.h>
#include <cpptools/cppmodelmanagerinterface.h>
#include <extensionsystem/pluginmanager.h>
#include <utils/algorithm.h>
#include <utils/qtcassert.h>
#include <utils/stringutils.h>
#include <utils/hostosinfo.h>
#include <coreplugin/icore.h>
#include <coreplugin/infobar.h>
#include <coreplugin/documentmanager.h>
#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/mimedatabase.h>
#include <coreplugin/variablemanager.h>

#include <QDebug>
#include <QDir>
#include <QFormLayout>
#include <QFileSystemWatcher>

using namespace CMakeProjectManager;
using namespace CMakeProjectManager::Internal;
using namespace ProjectExplorer;

// QtCreator CMake Generator wishlist:
// Which make targets we need to build to get all executables
// What is the make we need to call
// What is the actual compiler executable
// DEFINES

// Open Questions
// Who sets up the environment for cl.exe ? INCLUDEPATH and so on

// TODO This code taken from projectnodes.cpp and it marked as HACK. Wait for more clean solution.
static ProjectExplorer::FileType getFileType(const QFileInfo &file)
{
    using namespace ProjectExplorer;

    const Core::MimeType mt = Core::MimeDatabase::findByFile(file);
    if (!mt)
        return UnknownFileType;

    const QString typeName = mt.type();
    if (typeName == QLatin1String(ProjectExplorer::Constants::CPP_SOURCE_MIMETYPE)
        || typeName == QLatin1String(ProjectExplorer::Constants::C_SOURCE_MIMETYPE))
        return SourceType;
    if (typeName == QLatin1String(ProjectExplorer::Constants::CPP_HEADER_MIMETYPE)
        || typeName == QLatin1String(ProjectExplorer::Constants::C_HEADER_MIMETYPE))
        return HeaderType;
    if (typeName == QLatin1String(ProjectExplorer::Constants::RESOURCE_MIMETYPE))
        return ResourceType;
    if (typeName == QLatin1String(ProjectExplorer::Constants::FORM_MIMETYPE))
        return FormType;
    if (typeName == QLatin1String(ProjectExplorer::Constants::QML_MIMETYPE))
        return QMLType;
    return UnknownFileType;
}

// Make file node by file name
static ProjectExplorer::FileNode* fileToFileNode(const QString &fileName)
{
    // TODO
    ProjectExplorer::FileNode *node = 0;
    bool generated = false;
    QString onlyFileName = QFileInfo(fileName).fileName();
    if (   (onlyFileName.startsWith(QLatin1String("moc_")) && onlyFileName.endsWith(QLatin1String(".cxx")))
           || (onlyFileName.startsWith(QLatin1String("ui_")) && onlyFileName.endsWith(QLatin1String(".h")))
           || (onlyFileName.startsWith(QLatin1String("qrc_")) && onlyFileName.endsWith(QLatin1String(".cxx"))))
        generated = true;

    if (fileName.endsWith(QLatin1String("CMakeLists.txt")))
        node = new ProjectExplorer::FileNode(fileName, ProjectExplorer::ProjectFileType, false);
    else {
        ProjectExplorer::FileType fileType = getFileType(QFileInfo(fileName));
        node = new ProjectExplorer::FileNode(fileName, fileType, generated);
    }

    return node;
}


/*!
  \class CMakeProject
*/
CMakeProject::CMakeProject(CMakeManager *manager, const QString &fileName)
    : m_manager(manager),
      m_activeTarget(0),
      m_fileName(fileName),
      m_cbpUpdateProcess(0),
      m_watcher(new QFileSystemWatcher(this))
{
    setId(Constants::CMAKEPROJECT_ID);
    setProjectContext(Core::Context(CMakeProjectManager::Constants::PROJECTCONTEXT));
    setProjectLanguages(Core::Context(ProjectExplorer::Constants::LANG_CXX));

    m_projectName = QFileInfo(fileName).absoluteDir().dirName();

    m_file = new CMakeFile(this, fileName);
    m_rootNode = new CMakeProjectNode(this, m_fileName);

    connect(this, SIGNAL(buildTargetsChanged()),
            this, SLOT(updateRunConfigurations()));

    connect(m_watcher, SIGNAL(fileChanged(QString)), this, SLOT(fileChanged(QString)));
}

CMakeProject::~CMakeProject()
{
    m_codeModelFuture.cancel();
    delete m_rootNode;
}

void CMakeProject::fileChanged(const QString &fileName)
{
    Q_UNUSED(fileName)

    parseCMakeLists();
}

void CMakeProject::changeActiveBuildConfiguration(ProjectExplorer::BuildConfiguration *bc)
{
    if (!bc)
        return;

    CMakeBuildConfiguration *cmakebc = static_cast<CMakeBuildConfiguration *>(bc);

    // Pop up a dialog asking the user to rerun cmake
    QString cbpFile = CMakeManager::findCbpFile(QDir(bc->buildDirectory().toString()));
    QFileInfo cbpFileFi(cbpFile);
    CMakeOpenProjectWizard::Mode mode = CMakeOpenProjectWizard::Nothing;
    if (!cbpFileFi.exists()) {
        mode = CMakeOpenProjectWizard::NeedToCreate;
    } else {
        foreach (const QString &file, m_watchedFiles) {
            if (QFileInfo(file).lastModified() > cbpFileFi.lastModified()) {
                mode = CMakeOpenProjectWizard::NeedToUpdate;
                break;
            }
        }
    }

    if (mode != CMakeOpenProjectWizard::Nothing) {
        CMakeBuildInfo info(cmakebc);
        CMakeOpenProjectWizard copw(Core::ICore::mainWindow(), m_manager, mode, &info);
        copw.setArguments(cmakebc->cmakeParams());
        if (copw.exec() == QDialog::Accepted) {
            cmakebc->setCMakeParams(copw.arguments());
            cmakebc->setUseNinja(copw.useNinja()); // NeedToCreate can change the Ninja setting
        }
    }

    // reparse
    parseCMakeLists();
}

void CMakeProject::activeTargetWasChanged(Target *target)
{
    if (m_activeTarget) {
        disconnect(m_activeTarget, SIGNAL(activeBuildConfigurationChanged(ProjectExplorer::BuildConfiguration*)),
                   this, SLOT(changeActiveBuildConfiguration(ProjectExplorer::BuildConfiguration*)));
    }

    m_activeTarget = target;

    if (!m_activeTarget)
        return;

    connect(m_activeTarget, SIGNAL(activeBuildConfigurationChanged(ProjectExplorer::BuildConfiguration*)),
            this, SLOT(changeActiveBuildConfiguration(ProjectExplorer::BuildConfiguration*)));

    changeActiveBuildConfiguration(m_activeTarget->activeBuildConfiguration());
}

void CMakeProject::changeBuildDirectory(CMakeBuildConfiguration *bc, const QString &newBuildDirectory)
{
    bc->setBuildDirectory(Utils::FileName::fromString(newBuildDirectory));
    parseCMakeLists();
}

QString CMakeProject::shadowBuildDirectory(const QString &projectFilePath, const Kit *k, const QString &bcName)
{
    if (projectFilePath.isEmpty())
        return QString();
    QFileInfo info(projectFilePath);

    const QString projectName = QFileInfo(info.absolutePath()).fileName();
    ProjectExplorer::ProjectMacroExpander expander(projectFilePath, projectName, k, bcName);
    QDir projectDir = QDir(projectDirectory(Utils::FileName::fromString(projectFilePath)).toString());
    QString buildPath = Utils::expandMacros(Core::DocumentManager::buildDirectory(), &expander);
    return QDir::cleanPath(projectDir.absoluteFilePath(buildPath));
}

QStringList CMakeProject::getCXXFlagsFor(const CMakeBuildTarget &buildTarget)
{
    QString makeCommand = QDir::fromNativeSeparators(buildTarget.makeCommand);
    int startIndex = makeCommand.indexOf(QLatin1Char('\"'));
    int endIndex = makeCommand.indexOf(QLatin1Char('\"'), startIndex + 1);
    if (startIndex != -1 && endIndex != -1) {
        startIndex += 1;
        QString makefile = makeCommand.mid(startIndex, endIndex - startIndex);
        int slashIndex = makefile.lastIndexOf(QLatin1Char('/'));
        makefile.truncate(slashIndex);
        makefile.append(QLatin1String("/CMakeFiles/") + buildTarget.title + QLatin1String(".dir/flags.make"));
        QFile file(makefile);
        if (file.exists()) {
            file.open(QIODevice::ReadOnly | QIODevice::Text);
            QTextStream stream(&file);
            while (!stream.atEnd()) {
                QString line = stream.readLine().trimmed();
                if (line.startsWith(QLatin1String("CXX_FLAGS ="))) {
                    // Skip past =
                    return line.mid(11).trimmed().split(QLatin1Char(' '), QString::SkipEmptyParts);
                }
            }
        }
    }

    // Attempt to find build.ninja file and obtain FLAGS (CXX_FLAGS) from there if no suitable flags.make were
    // found
    // Get "all" target's working directory
    QString buildNinjaFile = QDir::fromNativeSeparators(buildTarget.workingDirectory);
    buildNinjaFile += QLatin1String("/build.ninja");
    QFile buildNinja(buildNinjaFile);
    if (buildNinja.exists()) {
        buildNinja.open(QIODevice::ReadOnly | QIODevice::Text);
        QTextStream stream(&buildNinja);
        bool cxxFound = false;
        while (!stream.atEnd()) {
            QString line = stream.readLine().trimmed();
            // Look for a build rule which invokes CXX_COMPILER
            if (line.startsWith(QLatin1String("build"))) {
                cxxFound = line.indexOf(QLatin1String("CXX_COMPILER")) != -1;
            } else if (cxxFound && line.startsWith(QLatin1String("FLAGS ="))) {
                // Skip past =
                return line.mid(7).trimmed().split(QLatin1Char(' '), QString::SkipEmptyParts);
            }
        }
    }
    return QStringList();
}

bool CMakeProject::parseCMakeLists()
{
    if (!activeTarget() ||
        !activeTarget()->activeBuildConfiguration()) {
        return false;
    }

    CMakeBuildConfiguration *activeBC = static_cast<CMakeBuildConfiguration *>(activeTarget()->activeBuildConfiguration());
    foreach (Core::IDocument *document, Core::DocumentModel::openedDocuments())
        if (isProjectFile(document->filePath()))
            document->infoBar()->removeInfo("CMakeEditor.RunCMake");

    // Find cbp file
    QString cbpFile = CMakeManager::findCbpFile(activeBC->buildDirectory().toString());

    if (cbpFile.isEmpty()) {
        emit buildTargetsChanged();
        return false;
    }

    // setFolderName
    m_rootNode->setDisplayName(QFileInfo(cbpFile).completeBaseName());
    CMakeCbpParser cbpparser;
    // Parsing
    //qDebug()<<"Parsing file "<<cbpFile;
    if (!cbpparser.parseCbpFile(cbpFile, projectDirectory().toString())) {
        // TODO report error
        emit buildTargetsChanged();
        return false;
    }

    foreach (const QString &file, m_watcher->files())
        if (file != cbpFile)
            m_watcher->removePath(file);

    // how can we ensure that it is completely written?
    m_watcher->addPath(cbpFile);

    m_projectName = cbpparser.projectName();
    m_rootNode->setDisplayName(cbpparser.projectName());

    //qDebug()<<"Building Tree";
    QList<ProjectExplorer::FileNode *> fileList = cbpparser.fileList(); // this files must be passed to code model
    QList<ProjectExplorer::FileNode *> treeFileList;                    // this files must be used to build source tree
    QSet<QString> projectFiles;

    // Take file list from file system instead cbp project file
    const QDir        dir(projectDirectory().toString());
    QStringList       sources, paths;
    getFileList(dir, projectDirectory().toString(), /*suffixes,*/ &sources, &paths);
    foreach (const QString &source, sources) {
        QFileInfo                  fileInfo(source);
        QString                    fileName = fileInfo.fileName();
        ProjectExplorer::FileNode *node     = fileToFileNode(source);

        if (fileName.endsWith(QLatin1String("CMakeLists.txt"))) {
            projectFiles.insert(source);
        } else {
            treeFileList.append(node);
        }
    }

    if (cbpparser.hasCMakeFiles()) {
        treeFileList.append(cbpparser.cmakeFileList());
        foreach (const ProjectExplorer::FileNode *node, cbpparser.cmakeFileList())
            projectFiles.insert(node->path());
    } else /*if (projectFiles.isEmpty())*/ {
        // Manually add the CMakeLists.txt file
        QString cmakeListTxt = projectDirectory().toString() + QLatin1String("/CMakeLists.txt");
        bool generated = false;
        treeFileList.append(new ProjectExplorer::FileNode(cmakeListTxt, ProjectExplorer::ProjectFileType, generated));
        projectFiles.insert(cmakeListTxt);
    }

    m_watchedFiles = projectFiles;

    m_files.clear();
    foreach (ProjectExplorer::FileNode *fn, fileList)
    {
        m_files.append(fn->path());
        delete fn;
    }
    m_files.sort();
    m_files.removeDuplicates();

    buildTree(m_rootNode, treeFileList);

    //qDebug()<<"Adding Targets";
    m_buildTargets = cbpparser.buildTargets();
//        qDebug()<<"Printing targets";
//        foreach (CMakeBuildTarget ct, m_buildTargets) {
//            qDebug()<<ct.title<<" with executable:"<<ct.executable;
//            qDebug()<<"WD:"<<ct.workingDirectory;
//            qDebug()<<ct.makeCommand<<ct.makeCleanCommand;
//            qDebug()<<"";
//        }

    updateApplicationAndDeploymentTargets();

    createUiCodeModelSupport();

    Kit *k = activeTarget()->kit();
    ToolChain *tc = ProjectExplorer::ToolChainKitInformation::toolChain(k);
    if (!tc) {
        emit buildTargetsChanged();
        emit fileListChanged();
        return true;
    }

    CppTools::CppModelManagerInterface *modelmanager =
            CppTools::CppModelManagerInterface::instance();
    if (modelmanager) {
        CppTools::CppModelManagerInterface::ProjectInfo pinfo = modelmanager->projectInfo(this);
        pinfo.clearProjectParts();

        foreach (const CMakeBuildTarget &cbt, m_buildTargets) {
            typedef CppTools::ProjectPart ProjectPart;
            ProjectPart::Ptr part(new ProjectPart);
            part->project = this;
            part->displayName = cbt.title;
            part->projectFile = projectFilePath().toString();

            // This explicitly adds -I. to the include paths
            part->headerPaths += ProjectPart::HeaderPath(projectDirectory().toString(),
                                                         ProjectPart::HeaderPath::IncludePath);
    //allIncludePaths.append(paths); // This want a lot of memory

            foreach (const QString &includeFile, cbt.includeFiles) {
                ProjectPart::HeaderPath hp(includeFile, ProjectPart::HeaderPath::IncludePath);

                // CodeBlocks is utterly ignorant of frameworks on Mac, and won't report framework
                // paths. The work-around is to check if the include path ends in ".framework", and
                // if so, add the parent directory as framework path.
                if (includeFile.endsWith(QLatin1String(".framework"))) {
                    const int slashIdx = includeFile.lastIndexOf(QLatin1Char('/'));
                    if (slashIdx != -1) {
                        hp = ProjectPart::HeaderPath(includeFile.left(slashIdx),
                                                     ProjectPart::HeaderPath::FrameworkPath);
                        continue;
                    }
                }

                part->headerPaths += hp;
            }

            part->projectDefines += cbt.defines;

            // TODO rewrite
            CppTools::ProjectFileAdder adder(part->files);
            foreach (const QString &file, cbt.files)
                adder.maybeAdd(file);

            QStringList cxxflags = getCXXFlagsFor(cbt);

            part->evaluateToolchain(tc,
                                    cxxflags,
                                    cxxflags,
                                    SysRootKitInformation::sysRoot(k));

            setProjectLanguage(ProjectExplorer::Constants::LANG_CXX, !part->files.isEmpty());
            pinfo.appendProjectPart(part);
        }
        m_codeModelFuture.cancel();
        m_codeModelFuture = modelmanager->updateProjectInfo(pinfo);

    }

    emit displayNameChanged();
    emit buildTargetsChanged();
    emit fileListChanged();

    emit activeBC->emitBuildTypeChanged();

    return true;
}

bool CMakeProject::isProjectFile(const QString &fileName)
{
    return m_watchedFiles.contains(fileName);
}

QList<CMakeBuildTarget> CMakeProject::buildTargets() const
{
    return m_buildTargets;
}

QStringList CMakeProject::buildTargetTitles(bool runnable) const
{
    QStringList results;
    foreach (const CMakeBuildTarget &ct, m_buildTargets) {
        if (runnable && (ct.executable.isEmpty() || ct.library))
            continue;
        results << ct.title;
    }
    return results;
}

bool CMakeProject::hasBuildTarget(const QString &title) const
{
    foreach (const CMakeBuildTarget &ct, m_buildTargets) {
        if (ct.title == title)
            return true;
    }
    return false;
}

void CMakeProject::gatherFileNodes(ProjectExplorer::FolderNode *parent, QList<ProjectExplorer::FileNode *> &list)
{
    foreach (ProjectExplorer::FolderNode *folder, parent->subFolderNodes())
        gatherFileNodes(folder, list);
    foreach (ProjectExplorer::FileNode *file, parent->fileNodes())
        list.append(file);
}

bool sortNodesByPath(Node *a, Node *b)
{
    return a->path() < b->path();
}

void CMakeProject::buildTree(CMakeProjectNode *rootNode, QList<ProjectExplorer::FileNode *> newList)
{
    // Gather old list
    QList<ProjectExplorer::FileNode *> oldList;
    gatherFileNodes(rootNode, oldList);
    Utils::sort(oldList, sortNodesByPath);
    Utils::sort(newList, sortNodesByPath);

    QList<ProjectExplorer::FileNode *> added;
    QList<ProjectExplorer::FileNode *> deleted;

    ProjectExplorer::compareSortedLists(oldList, newList, deleted, added, sortNodesByPath);

    qDeleteAll(ProjectExplorer::subtractSortedList(newList, added, sortNodesByPath));

    // add added nodes
    foreach (ProjectExplorer::FileNode *fn, added) {
//        qDebug()<<"added"<<fn->path();
        // Get relative path to rootNode
        QString parentDir = QFileInfo(fn->path()).absolutePath();
        ProjectExplorer::FolderNode *folder = findOrCreateFolder(rootNode, parentDir);
        folder->addFileNodes(QList<ProjectExplorer::FileNode *>()<< fn);
    }

    // remove old file nodes and check whether folder nodes can be removed
    foreach (ProjectExplorer::FileNode *fn, deleted) {
        ProjectExplorer::FolderNode *parent = fn->parentFolderNode();
//        qDebug()<<"removed"<<fn->path();
        parent->removeFileNodes(QList<ProjectExplorer::FileNode *>() << fn);
        // Check for empty parent
        while (parent->subFolderNodes().isEmpty() && parent->fileNodes().isEmpty()) {
            ProjectExplorer::FolderNode *grandparent = parent->parentFolderNode();
            grandparent->removeFolderNodes(QList<ProjectExplorer::FolderNode *>() << parent);
            parent = grandparent;
            if (parent == rootNode)
                break;
        }
    }
}

ProjectExplorer::FolderNode *CMakeProject::findOrCreateFolder(CMakeProjectNode *rootNode, QString directory)
{
    QString relativePath = QDir(QFileInfo(rootNode->path()).path()).relativeFilePath(directory);
    QStringList parts = relativePath.split(QLatin1Char('/'), QString::SkipEmptyParts);
    ProjectExplorer::FolderNode *parent = rootNode;
    QString path = QFileInfo(rootNode->path()).path();
    foreach (const QString &part, parts) {
        path += QLatin1Char('/');
        path += part;
        // Find folder in subFolders
        bool found = false;
        foreach (ProjectExplorer::FolderNode *folder, parent->subFolderNodes()) {
            if (folder->path() == path) {
                // yeah found something :)
                parent = folder;
                found = true;
                break;
            }
        }
        if (!found) {
            // No FolderNode yet, so create it
            ProjectExplorer::FolderNode *tmp = new ProjectExplorer::FolderNode(path);
            tmp->setDisplayName(part);
            parent->addFolderNodes(QList<ProjectExplorer::FolderNode *>() << tmp);
            parent = tmp;
        }
    }
    return parent;
}

QString CMakeProject::displayName() const
{
    return m_projectName;
}

Core::IDocument *CMakeProject::document() const
{
    return m_file;
}

CMakeManager *CMakeProject::projectManager() const
{
    return m_manager;
}

ProjectExplorer::ProjectNode *CMakeProject::rootProjectNode() const
{
    return m_rootNode;
}


QStringList CMakeProject::files(FilesMode fileMode) const
{
    Q_UNUSED(fileMode)
    return m_files;
}

bool CMakeProject::fromMap(const QVariantMap &map)
{
    if (!Project::fromMap(map))
        return false;

    bool hasUserFile = activeTarget();
    if (!hasUserFile) {
        CMakeOpenProjectWizard copw(Core::ICore::mainWindow(), m_manager, projectDirectory().toString(), Utils::Environment::systemEnvironment());
        if (copw.exec() != QDialog::Accepted)
            return false;
        Kit *k = copw.kit();
        Target *t = new Target(this, k);
        CMakeBuildConfiguration *bc(new CMakeBuildConfiguration(t));
        bc->setDefaultDisplayName(QLatin1String("all"));
        bc->setUseNinja(copw.useNinja());
        bc->setBuildDirectory(Utils::FileName::fromString(copw.buildDirectory()));
        ProjectExplorer::BuildStepList *buildSteps = bc->stepList(ProjectExplorer::Constants::BUILDSTEPS_BUILD);
        ProjectExplorer::BuildStepList *cleanSteps = bc->stepList(ProjectExplorer::Constants::BUILDSTEPS_CLEAN);

        // Now create a standard build configuration
        buildSteps->insertStep(0, new MakeStep(buildSteps));

        MakeStep *cleanMakeStep = new MakeStep(cleanSteps);
        cleanSteps->insertStep(0, cleanMakeStep);
        cleanMakeStep->setAdditionalArguments(QLatin1String("clean"));
        cleanMakeStep->setClean(true);

        t->addBuildConfiguration(bc);

        t->updateDefaultDeployConfigurations();

        addTarget(t);
    } else {
        // We have a user file, but we could still be missing the cbp file
        // or simply run createXml with the saved settings
        QFileInfo sourceFileInfo(m_fileName);
        CMakeBuildConfiguration *activeBC = qobject_cast<CMakeBuildConfiguration *>(activeTarget()->activeBuildConfiguration());
        if (!activeBC)
            return false;
        QString cbpFile = CMakeManager::findCbpFile(QDir(activeBC->buildDirectory().toString()));
        QFileInfo cbpFileFi(cbpFile);

        CMakeOpenProjectWizard::Mode mode = CMakeOpenProjectWizard::Nothing;
        if (!cbpFileFi.exists())
            mode = CMakeOpenProjectWizard::NeedToCreate;
        else if (cbpFileFi.lastModified() < sourceFileInfo.lastModified())
            mode = CMakeOpenProjectWizard::NeedToUpdate;

        if (mode != CMakeOpenProjectWizard::Nothing) {
            CMakeBuildInfo info(activeBC);
            CMakeOpenProjectWizard copw(Core::ICore::mainWindow(), m_manager, mode, &info);
        copw.setArguments(activeBC->cmakeParams());
            if (copw.exec() != QDialog::Accepted)
                return false;
            else {
                activeBC->setUseNinja(copw.useNinja());
                activeBC->setCMakeParams(copw.arguments());
            }
        }
    }

    parseCMakeLists();

    m_activeTarget = activeTarget();
    if (m_activeTarget)
        connect(m_activeTarget, SIGNAL(activeBuildConfigurationChanged(ProjectExplorer::BuildConfiguration*)),
                this, SLOT(changeActiveBuildConfiguration(ProjectExplorer::BuildConfiguration*)));

    connect(this, SIGNAL(activeTargetChanged(ProjectExplorer::Target*)),
            this, SLOT(activeTargetWasChanged(ProjectExplorer::Target*)));

    return true;
}

bool CMakeProject::setupTarget(Target *t)
{
    t->updateDefaultBuildConfigurations();
    if (t->buildConfigurations().isEmpty())
        return false;
    t->updateDefaultDeployConfigurations();

    return true;
}

CMakeBuildTarget CMakeProject::buildTargetForTitle(const QString &title)
{
    foreach (const CMakeBuildTarget &ct, m_buildTargets)
        if (ct.title == title)
            return ct;
    return CMakeBuildTarget();
}

QString CMakeProject::uiHeaderFile(const QString &uiFile)
{
    QFileInfo fi(uiFile);
    Utils::FileName project = projectDirectory();
    Utils::FileName baseDirectory = Utils::FileName::fromString(fi.absolutePath());

    while (baseDirectory.isChildOf(project)) {
        Utils::FileName cmakeListsTxt = baseDirectory;
        cmakeListsTxt.appendPath(QLatin1String("CMakeLists.txt"));
        if (cmakeListsTxt.toFileInfo().exists())
            break;
        QDir dir(baseDirectory.toString());
        dir.cdUp();
        baseDirectory = Utils::FileName::fromString(dir.absolutePath());
    }

    QDir srcDirRoot = QDir(project.toString());
    QString relativePath = srcDirRoot.relativeFilePath(baseDirectory.toString());
    QDir buildDir = QDir(activeTarget()->activeBuildConfiguration()->buildDirectory().toString());
    QString uiHeaderFilePath = buildDir.absoluteFilePath(relativePath);
    uiHeaderFilePath += QLatin1String("/ui_");
    uiHeaderFilePath += fi.completeBaseName();
    uiHeaderFilePath += QLatin1String(".h");

    return QDir::cleanPath(uiHeaderFilePath);
}

void CMakeProject::updateRunConfigurations()
{
    foreach (Target *t, targets())
        updateRunConfigurations(t);
}

void CMakeProject::cbpUpdateFinished(int /*code*/)
{
    if (m_cbpUpdateProcess->exitCode() != 0) {
        cbpUpdateMessage(tr("CMake exited with error. "
                            "Please run CMake wizard manualy and check output"));
    } else {
        refresh();
    }

    m_cbpUpdateProcess->deleteLater();
    m_cbpUpdateProcess = 0;
}

// TODO Compare with updateDefaultRunConfigurations();
void CMakeProject::updateRunConfigurations(Target *t)
{
    // *Update* runconfigurations:
    QMultiMap<QString, CMakeRunConfiguration*> existingRunConfigurations;
    QList<ProjectExplorer::RunConfiguration *> toRemove;
    foreach (ProjectExplorer::RunConfiguration *rc, t->runConfigurations()) {
        if (CMakeRunConfiguration* cmakeRC = qobject_cast<CMakeRunConfiguration *>(rc))
            existingRunConfigurations.insert(cmakeRC->title(), cmakeRC);
        QtSupport::CustomExecutableRunConfiguration *ceRC =
                qobject_cast<QtSupport::CustomExecutableRunConfiguration *>(rc);
        if (ceRC && !ceRC->isConfigured())
            toRemove << rc;
    }

    foreach (const CMakeBuildTarget &ct, buildTargets()) {
        if (ct.library)
            continue;
        if (ct.executable.isEmpty())
            continue;
        QList<CMakeRunConfiguration *> list = existingRunConfigurations.values(ct.title);
        if (!list.isEmpty()) {
            // Already exists, so override the settings...
            foreach (CMakeRunConfiguration *rc, list) {
                rc->setExecutable(ct.executable);
                rc->setBaseWorkingDirectory(ct.workingDirectory);
                rc->setEnabled(true);
            }
            existingRunConfigurations.remove(ct.title);
        } else {
            // Does not exist yet
            Core::Id id = CMakeRunConfigurationFactory::idFromBuildTarget(ct.title);
            CMakeRunConfiguration *rc = new CMakeRunConfiguration(t, id, ct.executable,
                                                                  ct.workingDirectory, ct.title);
            t->addRunConfiguration(rc);
        }
    }
    QMultiMap<QString, CMakeRunConfiguration *>::const_iterator it =
            existingRunConfigurations.constBegin();
    for ( ; it != existingRunConfigurations.constEnd(); ++it) {
        CMakeRunConfiguration *rc = it.value();
        // The executables for those runconfigurations aren't build by the current buildconfiguration
        // We just set a disable flag and show that in the display name
        rc->setEnabled(false);
        // removeRunConfiguration(rc);
    }

    foreach (ProjectExplorer::RunConfiguration *rc, toRemove)
        t->removeRunConfiguration(rc);

    if (t->runConfigurations().isEmpty()) {
        // Oh no, no run configuration,
        // create a custom executable run configuration
        t->addRunConfiguration(new QtSupport::CustomExecutableRunConfiguration(t));
    }
}

void CMakeProject::cbpUpdateMessage(const QString &message, bool show)
{
    Core::IDocument *document = Core::EditorManager::currentDocument();

    if (!document)
        return;

    Core::InfoBar *infoBar = document->infoBar();
    Core::Id id = Core::Id("CMakeProject.UpdateCbp");

    if (!infoBar->canInfoBeAdded(id))
        return;

    if (show) {
        Core::InfoBarEntry info(id, message, Core::InfoBarEntry::GlobalSuppressionEnabled);
        // TODO add custom buttor to run CMake Wizard
        //info.setCustomButtonInfo(tr("Reload QML"), this,
        //                         SLOT(reloadQml()));
        infoBar->addInfo(info);
    }
    else {
        infoBar->removeInfo(id);
    }
}

void CMakeProject::updateCbp()
{
    if (m_cbpUpdateProcess && m_cbpUpdateProcess->state() != QProcess::NotRunning)
        return;

    cbpUpdateMessage(QLatin1String(""), false);

    if (m_manager->isCMakeExecutableValid()) {
        m_cbpUpdateProcess = new Utils::QtcProcess();
        connect(m_cbpUpdateProcess, SIGNAL(finished(int)), this, SLOT(cbpUpdateFinished(int)));

        CMakeBuildConfiguration *bc
            = static_cast<CMakeBuildConfiguration *>(activeTarget()->activeBuildConfiguration());

        CMakeProjectManager::Internal::GeneratorInfo generatorInfo(bc->target()->kit(),
                                                                   bc->useNinja());

        m_manager->createXmlFile(m_cbpUpdateProcess,
                                 bc->cmakeParams(),
                                 bc->target()->project()->projectDirectory().toString(),
                                 bc->buildDirectory().toString(),
                                 bc->environment(),
                                 QString::fromLatin1(generatorInfo.generatorArgument()));
    } else {
        cbpUpdateMessage(tr("No valid cmake executable specified."));
    }

}

void CMakeProject::updateApplicationAndDeploymentTargets()
{
    Target *t = activeTarget();

    QFile deploymentFile;
    QTextStream deploymentStream;
    QString deploymentPrefix;
    QDir sourceDir;

    sourceDir.setPath(t->project()->projectDirectory().toString());
    deploymentFile.setFileName(sourceDir.filePath(QLatin1String("QtCreatorDeployment.txt")));
    if (deploymentFile.open(QFile::ReadOnly | QFile::Text)) {
        deploymentStream.setDevice(&deploymentFile);
        deploymentPrefix = deploymentStream.readLine();
        if (!deploymentPrefix.endsWith(QLatin1Char('/')))
            deploymentPrefix.append(QLatin1Char('/'));
    }

    BuildTargetInfoList appTargetList;
    DeploymentData deploymentData;
    QDir buildDir(t->activeBuildConfiguration()->buildDirectory().toString());
    foreach (const CMakeBuildTarget &ct, m_buildTargets) {
        if (ct.executable.isEmpty())
            continue;

        deploymentData.addFile(ct.executable, deploymentPrefix + buildDir.relativeFilePath(QFileInfo(ct.executable).dir().path()), DeployableFile::TypeExecutable);
        if (!ct.library) {
            // TODO: Put a path to corresponding .cbp file into projectFilePath?
            appTargetList.list << BuildTargetInfo(ct.executable, ct.executable);
        }
    }

    QString absoluteSourcePath = sourceDir.absolutePath();
    if (!absoluteSourcePath.endsWith(QLatin1Char('/')))
        absoluteSourcePath.append(QLatin1Char('/'));
    if (deploymentStream.device()) {
        while (!deploymentStream.atEnd()) {
            QString line = deploymentStream.readLine();
            if (!line.contains(QLatin1Char(':')))
                continue;
            QStringList file = line.split(QLatin1Char(':'));
            deploymentData.addFile(absoluteSourcePath + file.at(0), deploymentPrefix + file.at(1));
        }
    }

    t->setApplicationTargets(appTargetList);
    t->setDeploymentData(deploymentData);
}

void CMakeProject::createUiCodeModelSupport()
{
    QHash<QString, QString> uiFileHash;

    // Find all ui files
    foreach (const QString &uiFile, m_files) {
        if (uiFile.endsWith(QLatin1String(".ui")))
            uiFileHash.insert(uiFile, uiHeaderFile(uiFile));
    }

    QtSupport::UiCodeModelManager::update(this, uiFileHash);
}

// CMakeFile

CMakeFile::CMakeFile(CMakeProject *parent, QString fileName)
    : Core::IDocument(parent), m_project(parent)
{
    setId("Cmake.ProjectFile");
    setMimeType(QLatin1String(Constants::CMAKEPROJECTMIMETYPE));
    setFilePath(fileName);
}

bool CMakeFile::save(QString *errorString, const QString &fileName, bool autoSave)
{
    // Once we have an texteditor open for this file, we probably do
    // need to implement this, don't we.
    Q_UNUSED(errorString)
    Q_UNUSED(fileName)
    Q_UNUSED(autoSave)
    return false;
}

QString CMakeFile::defaultPath() const
{
    return QString();
}

QString CMakeFile::suggestedFileName() const
{
    return QString();
}

bool CMakeFile::isModified() const
{
    return false;
}

bool CMakeFile::isSaveAsAllowed() const
{
    return false;
}

Core::IDocument::ReloadBehavior CMakeFile::reloadBehavior(ChangeTrigger state, ChangeType type) const
{
    Q_UNUSED(state)
    Q_UNUSED(type)
    return BehaviorSilent;
}

bool CMakeFile::reload(QString *errorString, ReloadFlag flag, ChangeType type)
{
    Q_UNUSED(errorString)
    Q_UNUSED(flag)
    Q_UNUSED(type)
    return true;
}

CMakeBuildSettingsWidget::CMakeBuildSettingsWidget(CMakeBuildConfiguration *bc) : m_buildConfiguration(0)
{
    QFormLayout *fl = new QFormLayout(this);
    fl->setContentsMargins(20, -1, 0, -1);
    fl->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    setLayout(fl);

    QPushButton *runCmakeButton = new QPushButton(tr("Run CMake..."));
    connect(runCmakeButton, SIGNAL(clicked()),
            this, SLOT(runCMake()));
    fl->addRow(tr("Reconfigure project:"), runCmakeButton);

    m_pathLineEdit = new QLineEdit(this);
    m_pathLineEdit->setReadOnly(true);

    QHBoxLayout *hbox = new QHBoxLayout();
    hbox->addWidget(m_pathLineEdit);

    m_changeButton = new QPushButton(this);
    m_changeButton->setText(tr("&Change"));
    connect(m_changeButton, SIGNAL(clicked()), this, SLOT(openChangeBuildDirectoryDialog()));
    hbox->addWidget(m_changeButton);

    fl->addRow(tr("Build directory:"), hbox);

    m_buildConfiguration = bc;
    m_pathLineEdit->setText(m_buildConfiguration->rawBuildDirectory().toString());
    if (m_buildConfiguration->buildDirectory() == bc->target()->project()->projectDirectory())
        m_changeButton->setEnabled(false);
    else
        m_changeButton->setEnabled(true);

    setDisplayName(tr("CMake"));
}

void CMakeBuildSettingsWidget::openChangeBuildDirectoryDialog()
{
    CMakeProject *project = static_cast<CMakeProject *>(m_buildConfiguration->target()->project());
    CMakeBuildInfo info(m_buildConfiguration);
    CMakeOpenProjectWizard copw(Core::ICore::mainWindow(),
                                project->projectManager(), CMakeOpenProjectWizard::ChangeDirectory,
                                &info);
    copw.setArguments(m_buildConfiguration->cmakeParams());
    if (copw.exec() == QDialog::Accepted) {
        project->changeBuildDirectory(m_buildConfiguration, copw.buildDirectory());
        m_buildConfiguration->setUseNinja(copw.useNinja());
        m_pathLineEdit->setText(m_buildConfiguration->rawBuildDirectory().toString());
        m_buildConfiguration->setCMakeParams(copw.arguments());
    }
}

void CMakeBuildSettingsWidget::runCMake()
{
    if (!ProjectExplorer::ProjectExplorerPlugin::instance()->saveModifiedFiles())
        return;
    CMakeProject *project = static_cast<CMakeProject *>(m_buildConfiguration->target()->project());
    CMakeBuildInfo info(m_buildConfiguration);
    CMakeOpenProjectWizard copw(Core::ICore::mainWindow(),
                                project->projectManager(),
                                CMakeOpenProjectWizard::WantToUpdate, &info);
    copw.setArguments(m_buildConfiguration->cmakeParams());
    if (copw.exec() == QDialog::Accepted) {
        project->parseCMakeLists();
        m_buildConfiguration->setCMakeParams(copw.arguments());
    }
}

/////
// CMakeCbpParser
////

// called after everything is parsed
// this function tries to figure out to which CMakeBuildTarget
// each file belongs, so that it gets the appropriate defines and
// compiler flags
void CMakeCbpParser::sortFiles()
{
    QList<Utils::FileName> fileNames = Utils::transform(m_fileList, [] (FileNode *node) {
        return Utils::FileName::fromString(node->path());
    });

    Utils::sort(fileNames);


    CMakeBuildTarget *last = 0;
    Utils::FileName parentDirectory;

    foreach (const Utils::FileName &fileName, fileNames) {
        if (fileName.parentDir() == parentDirectory && last) {
            // easy case, same parent directory as last file
            last->files.append(fileName.toString());
        } else {
            int bestLength = -1;
            int bestIndex = -1;

            for (int i = 0; i < m_buildTargets.size(); ++i) {
                const CMakeBuildTarget &target = m_buildTargets.at(i);
                if (fileName.isChildOf(Utils::FileName::fromString(target.sourceDirectory))
                                       && target.sourceDirectory.size() > bestLength) {
                    bestLength = target.sourceDirectory.size();
                    bestIndex = i;
                }
            }

            if (bestIndex == -1 && !m_buildTargets.isEmpty())
                bestIndex = 0;

            if (bestIndex != -1) {
                m_buildTargets[bestIndex].files.append(fileName.toString());
                last = &m_buildTargets[bestIndex];
                parentDirectory = fileName.parentDir();
            }
        }
    }
}

bool CMakeCbpParser::parseCbpFile(const QString &fileName, const QString &sourceDirectory)
{
    m_buildDirectory = QFileInfo(fileName).absolutePath();
    m_sourceDirectory = sourceDirectory;

    QFile fi(fileName);
    if (fi.exists() && fi.open(QFile::ReadOnly)) {
        setDevice(&fi);

        while (!atEnd()) {
            readNext();
            if (name() == QLatin1String("CodeBlocks_project_file"))
                parseCodeBlocks_project_file();
            else if (isStartElement())
                parseUnknownElement();
        }

        sortFiles();

        fi.close();
        return true;
    }
    return false;
}

void CMakeCbpParser::parseCodeBlocks_project_file()
{
    while (!atEnd()) {
        readNext();
        if (isEndElement())
            return;
        else if (name() == QLatin1String("Project"))
            parseProject();
        else if (isStartElement())
            parseUnknownElement();
    }
}

void CMakeCbpParser::parseProject()
{
    while (!atEnd()) {
        readNext();
        if (isEndElement())
            return;
        else if (name() == QLatin1String("Option"))
            parseOption();
        else if (name() == QLatin1String("Unit"))
            parseUnit();
        else if (name() == QLatin1String("Build"))
            parseBuild();
        else if (isStartElement())
            parseUnknownElement();
    }
}

void CMakeCbpParser::parseBuild()
{
    while (!atEnd()) {
        readNext();
        if (isEndElement())
            return;
        else if (name() == QLatin1String("Target"))
            parseBuildTarget();
        else if (isStartElement())
            parseUnknownElement();
    }
}

void CMakeCbpParser::parseBuildTarget()
{
    m_buildTarget.clear();

    if (attributes().hasAttribute(QLatin1String("title")))
        m_buildTarget.title = attributes().value(QLatin1String("title")).toString();
    while (!atEnd()) {
        readNext();
        if (isEndElement()) {
            if (!m_buildTarget.title.endsWith(QLatin1String("/fast")))
                m_buildTargets.append(m_buildTarget);
            return;
        } else if (name() == QLatin1String("Compiler")) {
            parseCompiler();
        } else if (name() == QLatin1String("Option")) {
            parseBuildTargetOption();
        } else if (name() == QLatin1String("MakeCommands")) {
            parseMakeCommands();
        } else if (isStartElement()) {
            parseUnknownElement();
        }
    }
}

void CMakeCbpParser::parseBuildTargetOption()
{
    if (attributes().hasAttribute(QLatin1String("output"))) {
        m_buildTarget.executable = attributes().value(QLatin1String("output")).toString();
    } else if (attributes().hasAttribute(QLatin1String("type"))) {
        const QStringRef value = attributes().value(QLatin1String("type"));
        if (value == QLatin1String("2") || value == QLatin1String("3"))
            m_buildTarget.library = true;
    } else if (attributes().hasAttribute(QLatin1String("working_dir"))) {
        m_buildTarget.workingDirectory = attributes().value(QLatin1String("working_dir")).toString();
        QDir dir(m_buildDirectory);
        QString relative = dir.relativeFilePath(m_buildTarget.workingDirectory);
        m_buildTarget.sourceDirectory
                = Utils::FileName::fromString(m_sourceDirectory).appendPath(relative).toString();
    }
    while (!atEnd()) {
        readNext();
        if (isEndElement())
            return;
        else if (isStartElement())
            parseUnknownElement();
    }
}

QString CMakeCbpParser::projectName() const
{
    return m_projectName;
}

void CMakeCbpParser::parseOption()
{
    if (attributes().hasAttribute(QLatin1String("title")))
        m_projectName = attributes().value(QLatin1String("title")).toString();

    if (attributes().hasAttribute(QLatin1String("compiler")))
        m_compiler = attributes().value(QLatin1String("compiler")).toString();

    while (!atEnd()) {
        readNext();
        if (isEndElement())
            return;
        else if (isStartElement())
            parseUnknownElement();
    }
}

void CMakeCbpParser::parseMakeCommands()
{
    while (!atEnd()) {
        readNext();
        if (isEndElement())
            return;
        else if (name() == QLatin1String("Build"))
            parseBuildTargetBuild();
        else if (name() == QLatin1String("Clean"))
            parseBuildTargetClean();
        else if (isStartElement())
            parseUnknownElement();
    }
}

void CMakeCbpParser::parseBuildTargetBuild()
{
    if (attributes().hasAttribute(QLatin1String("command")))
        m_buildTarget.makeCommand = attributes().value(QLatin1String("command")).toString();
    while (!atEnd()) {
        readNext();
        if (isEndElement())
            return;
        else if (isStartElement())
            parseUnknownElement();
    }
}

void CMakeCbpParser::parseBuildTargetClean()
{
    if (attributes().hasAttribute(QLatin1String("command")))
        m_buildTarget.makeCleanCommand = attributes().value(QLatin1String("command")).toString();
    while (!atEnd()) {
        readNext();
        if (isEndElement())
            return;
        else if (isStartElement())
            parseUnknownElement();
    }
}

void CMakeCbpParser::parseCompiler()
{
    while (!atEnd()) {
        readNext();
        if (isEndElement())
            return;
        else if (name() == QLatin1String("Add"))
            parseAdd();
        else if (isStartElement())
            parseUnknownElement();
    }
}

void CMakeCbpParser::parseAdd()
{
    // CMake only supports <Add option=\> and <Add directory=\>
    const QXmlStreamAttributes addAttributes = attributes();

    const QString includeDirectory = addAttributes.value(QLatin1String("directory")).toString();
    // allow adding multiple times because order happens
    if (!includeDirectory.isEmpty())
        m_buildTarget.includeFiles.append(includeDirectory);

    QString compilerOption = addAttributes.value(QLatin1String("option")).toString();
    // defining multiple times a macro to the same value makes no sense
    if (!compilerOption.isEmpty() && !m_buildTarget.compilerOptions.contains(compilerOption)) {
        m_buildTarget.compilerOptions.append(compilerOption);
        int macroNameIndex = compilerOption.indexOf(QLatin1String("-D")) + 2;
        if (macroNameIndex != 1) {
            int assignIndex = compilerOption.indexOf(QLatin1Char('='), macroNameIndex);
            if (assignIndex != -1)
                compilerOption[assignIndex] = ' ';
            m_buildTarget.defines.append("#define ");
            m_buildTarget.defines.append(compilerOption.mid(macroNameIndex).toUtf8());
            m_buildTarget.defines.append('\n');
        }
    }

    while (!atEnd()) {
        readNext();
        if (isEndElement())
            return;
        else if (isStartElement())
            parseUnknownElement();
    }
}

void CMakeCbpParser::parseUnit()
{
    //qDebug()<<stream.attributes().value("filename");
    QString fileName = attributes().value(QLatin1String("filename")).toString();
    m_parsingCmakeUnit = false;
    while (!atEnd()) {
        readNext();
        if (isEndElement()) {
            if (!fileName.endsWith(QLatin1String(".rule")) && !m_processedUnits.contains(fileName)) {
                // Now check whether we found a virtual element beneath
                if (m_parsingCmakeUnit) {
                    m_cmakeFileList.append( new ProjectExplorer::FileNode(fileName, ProjectExplorer::ProjectFileType, false));
                } else {
                    m_fileList.append(fileToFileNode(fileName));
                }
                m_processedUnits.insert(fileName);
            }
            return;
        } else if (name() == QLatin1String("Option")) {
            parseUnitOption();
        } else if (isStartElement()) {
            parseUnknownElement();
        }
    }
}

void CMakeCbpParser::parseUnitOption()
{
    if (attributes().hasAttribute(QLatin1String("virtualFolder")))
        m_parsingCmakeUnit = true;

    while (!atEnd()) {
        readNext();

        if (isEndElement())
            break;

        if (isStartElement())
            parseUnknownElement();
    }
}

void CMakeCbpParser::parseUnknownElement()
{
    Q_ASSERT(isStartElement());

    while (!atEnd()) {
        readNext();

        if (isEndElement())
            break;

        if (isStartElement())
            parseUnknownElement();
    }
}

QList<ProjectExplorer::FileNode *> CMakeCbpParser::fileList()
{
    return m_fileList;
}

QList<ProjectExplorer::FileNode *> CMakeCbpParser::cmakeFileList()
{
    return m_cmakeFileList;
}

bool CMakeCbpParser::hasCMakeFiles()
{
    return !m_cmakeFileList.isEmpty();
}

QList<CMakeBuildTarget> CMakeCbpParser::buildTargets()
{
    return m_buildTargets;
}

QString CMakeCbpParser::compilerName() const
{
    return m_compiler;
}

void CMakeBuildTarget::clear()
{
    executable.clear();
    makeCommand.clear();
    makeCleanCommand.clear();
    workingDirectory.clear();
    sourceDirectory.clear();
    title.clear();
    library = false;
    includeFiles.clear();
    compilerOptions.clear();
    defines.clear();
}

void CMakeProject::getFileList(const QDir &dir,
                               const QString &projectRoot,
                               QStringList *files, QStringList *paths) const
{
    const QFileInfoList fileInfoList = dir.entryInfoList(QDir::Files |
                                                         QDir::Dirs |
                                                         QDir::NoDotAndDotDot |
                                                         QDir::NoSymLinks);

    foreach (const QFileInfo &fileInfo, fileInfoList) {
        QString filePath = fileInfo.absoluteFilePath();

        if (fileInfo.isDir() && isValidDir(fileInfo)) {
            getFileList(QDir(fileInfo.absoluteFilePath()), projectRoot,
                        files, paths);

            if (! paths->contains(filePath))
                paths->append(filePath);
        } else {
            files->append(filePath);
        }
    }
}

bool CMakeProject::isValidDir(const QFileInfo &fileInfo) const
{
    const QString fileName = fileInfo.fileName();
    const QString suffix = fileInfo.suffix();

    if (fileName.startsWith(QLatin1Char('.')))
        return false;

    else if (fileName == QLatin1String("CVS"))
        return false;

    // ### user include/exclude

    return true;
}

void CMakeProject::refresh()
{
    parseCMakeLists();
}

bool CMakeProject::addFiles(const QStringList &filePaths)
{
    Q_UNUSED(filePaths);
    updateCbp();
    return true;
}

bool CMakeProject::eraseFiles(const QStringList &filePaths)
{
    Q_UNUSED(filePaths);
    updateCbp();
    return true;
}

bool CMakeProjectManager::Internal::CMakeProject::renameFile(const QString &filePath, const QString &newFilePath)
{
    Q_UNUSED(filePath);
    Q_UNUSED(newFilePath);
    updateCbp();
    return true;
}
