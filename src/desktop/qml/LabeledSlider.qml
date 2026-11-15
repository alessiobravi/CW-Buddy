import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: root

    required property string caption
    property real from: 0
    property real to: 100
    property real value: 0
    property real stepSize: 1
    property int decimals: 0
    signal moved(real value)

    spacing: 1
    Label {
        text: root.caption + "  " + Number(root.value).toFixed(root.decimals)
        color: root.enabled ? "#c7d2df" : "#667586"
        font.pixelSize: 11
    }
    Slider {
        id: slider
        Layout.fillWidth: true
        from: root.from
        to: root.to
        stepSize: root.stepSize
        snapMode: Slider.SnapAlways
        onMoved: root.moved(value)
        ToolTip.visible: hovered
        ToolTip.text: root.caption + ": "
                      + Number(value).toFixed(root.decimals)
    }
    // The handle's position is pushed here instead of being declared as
    // `value: root.value` on the Slider itself.
    //
    // A dragged Slider writes its own `value`, and a QML property that has
    // been written to keeps the written value: the declarative binding is
    // gone for the rest of that control's life. From the first drag onwards
    // the handle no longer follows root.value, so whenever the setting it
    // edits is corrected on the way through C++ -- rounded, clamped to a
    // ceiling that depends on another setting, or refused outright -- the
    // caption above (which does still read root.value) and the handle show
    // two different numbers, with no way for the operator to tell which one
    // the application is using. The same component is instantiated three
    // times over for several of these settings, in this file's callers, so
    // the same value can also be left disagreeing with itself across panes.
    //
    // A Binding object survives that, because it re-asserts its expression on
    // the target property every time the expression changes, rather than
    // being a one-time binding the target can overwrite. It is preferred here
    // over an `onValueChanged: slider.value = root.value` re-sync because it
    // covers the initial value and every later change with one declaration --
    // the handwritten re-sync needs a Component.onCompleted beside it to set
    // the handle up in the first place, and two mechanisms that have to agree
    // is the shape of the bug being fixed. restoreMode is stated rather than
    // left to the default so that what happens to the handle when this
    // binding is torn down is written down, not inherited.
    Binding {
        target: slider
        property: "value"
        value: root.value
        restoreMode: Binding.RestoreBindingOrValue
    }
}
