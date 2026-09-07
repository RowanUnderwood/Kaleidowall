#include "window.h"
#include <QApplication>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QtTest>
using namespace kaleido;

class PresetTests : public QObject {
    Q_OBJECT
    void seed(const QString& path) {
        Library lib(path + "/library.sqlite");
        Settings s;
        s.maxSlots = 8;
        for (const auto& name : {"Alpha", "Beta", "Gamma"}) {
            s.minSlots = lib.presets().size() + 1;
            QVERIFY(lib.setValue("preset:" + QString(name), s.json()));
        }
        QVERIFY(lib.setValue("settings", lib.value("preset:Beta")));
        QVERIFY(lib.setValue("presetState", {{"loadedPreset", "Beta"}}));
    }
    QComboBox* combo(Window& w) { return w.findChild<QComboBox*>("presets"); }
    void select(Window& w, const QString& name) {
        const int index = combo(w)->findData(name);
        QVERIFY(index >= 0);
        combo(w)->setCurrentIndex(index);
    }
    void click(Window& w, const char* name) {
        auto* b = w.findChild<QPushButton*>(name);
        QVERIFY(b && b->isEnabled());
        b->click();
    }
    void confirm(Window& w, const char* name, QMessageBox::StandardButton answer) {
        bool answered = false;
        QTimer::singleShot(0, &w, [&] {
            auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
            if (dialog && dialog->button(answer)) {
                answered = true;
                dialog->button(answer)->click();
            }
        });
        click(w, name);
        QApplication::processEvents();
        QVERIFY(answered);
    }
  private slots:
    void saveConfirmsAndIncludesUnappliedEdits() {
        QTemporaryDir dir;
        seed(dir.path());
        Window w(dir.path());
        select(w, "Beta");
        w.findChild<QSpinBox*>("minSlots")->setValue(5);
        confirm(w, "presetSave", QMessageBox::Cancel);
        QCOMPARE(w.library()->value("preset:Beta").value("minSlots").toInt(), 2);
        QCOMPARE(w.canvas()->settings().minSlots, 2);
        confirm(w, "presetSave", QMessageBox::Yes);
        QCOMPARE(w.library()->value("preset:Beta").value("minSlots").toInt(), 5);
        QCOMPARE(w.canvas()->settings().minSlots, 5);
        QCOMPARE(combo(w)->currentData().toString(), QString("Beta"));
    }
    void deletingLoadedUsesNextThenPreviousAndProtectsLast() {
        QTemporaryDir dir;
        seed(dir.path());
        Window w(dir.path());
        select(w, "Beta");
        confirm(w, "presetDelete", QMessageBox::Cancel);
        QCOMPARE(w.library()->presets().size(), 3);
        confirm(w, "presetDelete", QMessageBox::Yes);
        QCOMPARE(combo(w)->currentData().toString(), QString("Gamma"));
        QCOMPARE(w.canvas()->settings().minSlots, 3);
        QCOMPARE(w.library()->value("presetState").value("loadedPreset").toString(), QString("Gamma"));
        confirm(w, "presetDelete", QMessageBox::Yes);
        QCOMPARE(combo(w)->currentData().toString(), QString("Alpha"));
        QCOMPARE(w.canvas()->settings().minSlots, 1);
        confirm(w, "presetDelete", QMessageBox::Ok);
        QCOMPARE(w.library()->presets(), QStringList{"Alpha"});
        QVERIFY(!w.library()->removePreset("Alpha"));
        QCOMPARE(w.library()->presets(), QStringList{"Alpha"});
    }
    void deletingSelectedButUnloadedLeavesSettingsAndEditsAlone() {
        QTemporaryDir dir;
        seed(dir.path());
        Window w(dir.path());
        select(w, "Alpha");
        w.findChild<QSpinBox*>("minSlots")->setValue(6);
        confirm(w, "presetDelete", QMessageBox::Yes);
        QCOMPARE(w.canvas()->settings().minSlots, 2);
        QCOMPARE(w.findChild<QSpinBox*>("minSlots")->value(), 6);
        QCOMPARE(w.library()->value("presetState").value("loadedPreset").toString(), QString("Beta"));
    }
    void defaultUsesSavedSettingsAtStartupAndClearsOnDeletion() {
        QTemporaryDir dir;
        seed(dir.path());
        {
            Window w(dir.path());
            select(w, "Alpha");
            w.findChild<QSpinBox*>("minSlots")->setValue(6);
            click(w, "presetDefault");
            QCOMPARE(w.canvas()->settings().minSlots, 2);
            QCOMPARE(w.findChild<QSpinBox*>("minSlots")->value(), 6);
            QVERIFY(combo(w)->currentText().contains("(default)"));
        }
        {
            Window w(dir.path());
            QCOMPARE(w.canvas()->settings().minSlots, 1);
            QCOMPARE(combo(w)->currentData().toString(), QString("Alpha"));
            w.findChild<QSpinBox*>("minSlots")->setValue(4);
            confirm(w, "presetSave", QMessageBox::Yes);
        }
        {
            Window w(dir.path());
            QCOMPARE(w.canvas()->settings().minSlots, 4);
            confirm(w, "presetDelete", QMessageBox::Yes);
            QVERIFY(w.library()->value("presetState").value("defaultPreset").toString().isEmpty());
            QCOMPARE(w.canvas()->settings().minSlots, 2);
        }
        Window w(dir.path());
        QCOMPARE(w.canvas()->settings().minSlots, 2);
        QCOMPARE(w.library()->presets(), (QStringList{"Beta", "Gamma"}));
    }
};
QTEST_MAIN(PresetTests)
#include "preset_tests.moc"
