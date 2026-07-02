import tkinter as tk

class ToolTip:
    """
    A simple hover tooltip for tkinter widgets.
    """
    def __init__(self, widget):
        self.widget = widget
        self.tip_window = None
        self.text = ""
        self.widget.bind("<Enter>", self.enter)
        self.widget.bind("<Leave>", self.leave)

    def enter(self, event=None):
        if not self.text:
            return
        self.schedule_show()

    def leave(self, event=None):
        self.unschedule()
        self.hide()

    def schedule_show(self):
        self.unschedule()
        self.id = self.widget.after(300, self.show)

    def unschedule(self):
        id_ = getattr(self, "id", None)
        if id_:
            self.widget.after_cancel(id_)
            self.id = None

    def show(self):
        if self.tip_window or not self.text:
            return
        x, y, cx, cy = self.widget.bbox("insert") or (0, 0, 0, 0)
        x = x + self.widget.winfo_rootx() + 25
        y = y + cy + self.widget.winfo_rooty() + 20
        self.tip_window = tw = tk.Toplevel(self.widget)
        tw.wm_overrideredirect(True)
        tw.wm_geometry("+%d+%d" % (x, y))
        label = tk.Label(tw, text=self.text, justify='left',
                         background="#ffffe0", relief='solid', borderwidth=1,
                         font=("TkDefaultFont", 8, "normal"))
        label.pack(ipadx=2, ipady=2)

    def hide(self):
        tw = self.tip_window
        self.tip_window = None
        if tw:
            tw.destroy()
