import QtQuick
import Quickshell
import qs.Ui

BarWidget {
  id: root
  moduleName: "osman.whatsapp-tui"

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: "\uf232"
    tooltipText: "WhatsApp TUI"
    onPressed: {
      if (root.bar)
        root.bar.run(Quickshell.env("HOME") + "/.local/bin/whatsapp-tui-launch")
    }
  }
}
