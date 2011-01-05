#include "PageView.h"
#include "XournalWidget.h"
#include "../gettext.h"
#include <stdlib.h>
#include <math.h>
#include <gdk/gdkkeysyms.h>
#include "../control/Control.h"
#include "../view/TextView.h"
#include "../control/ShapeRecognizer.h"

#define PIXEL_MOTION_THRESHOLD 0.3

//#define INPUT_DEBUG

PageView::PageView(XournalWidget * xournal, XojPage * page) {
	this->page = page;
	this->xournal = xournal;
	this->selected = false;
	this->tmpStroke = NULL;
	this->tmpStrokeDrawElem = 0;
	this->settings = xournal->getControl()->getSettings();
	this->view = new DocumentView();

	this->lastMousePositionX = 0;
	this->lastMousePositionY = 0;
	this->inScrolling = false;

	this->firstPainted = false;

	this->crBuffer = NULL;

	this->idleRepaintId = 0;

	this->inEraser = false;
	this->eraseDeleteUndoAction = NULL;
	this->eraseUndoAction = NULL;

	this->extendedWarningDisplayd = false;

	this->selectionEdit = NULL;
	this->selection = NULL;
	widget = gtk_drawing_area_new();
	gtk_widget_show(widget);

	this->textEditor = NULL;

	this->search = NULL;

	updateSize();

	gtk_widget_set_events(widget, GDK_EXPOSURE_MASK | GDK_POINTER_MOTION_MASK | GDK_BUTTON_MOTION_MASK
			| GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK);

	gtk_widget_set_can_focus(widget, true);

	g_signal_connect(widget, "button_press_event", G_CALLBACK(onButtonPressEventCallback), this);
	g_signal_connect(widget, "button_release_event", G_CALLBACK(onButtonReleaseEventCallback), this);
	g_signal_connect(widget, "motion_notify_event", G_CALLBACK(onMotionNotifyEventCallback), this);

	g_signal_connect(G_OBJECT(widget), "expose_event", G_CALLBACK(exposeEventCallback), this);

	updateXEvents();
}

PageView::~PageView() {
	gtk_widget_destroy(widget);
	delete this->view;
	endText();
	deleteViewBuffer();
}

void PageView::deleteViewBuffer() {
	if (crBuffer) {
		cairo_surface_destroy(crBuffer);
		crBuffer = NULL;
	}
}

/**
 * Change event handling between XInput and Core
 */
void PageView::updateXEvents() {
	if (!gtk_check_version(2, 17, 0)) {
		/* GTK+ 2.17 and later: everybody shares a single native window,
		 so we'll never get any core events, and we might as well set
		 extension events the way we're supposed to. Doing so helps solve
		 crasher bugs in 2.17, and prevents us from losing two-button
		 events in 2.18 */
		gtk_widget_set_extension_events(widget, settings->isUseXInput() ? GDK_EXTENSION_EVENTS_ALL
				: GDK_EXTENSION_EVENTS_NONE);
	} else {
		/* GTK+ 2.16 and earlier: we only activate extension events on the
		 PageViews's parent GdkWindow. This allows us to keep receiving core
		 events. */
		gdk_input_set_extension_events(widget->window, GDK_POINTER_MOTION_MASK | GDK_BUTTON_MOTION_MASK
				| GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK, settings->isUseXInput() ? GDK_EXTENSION_EVENTS_ALL
				: GDK_EXTENSION_EVENTS_NONE);
	}

}

gboolean PageView::onButtonPressEventCallback(GtkWidget *widget, GdkEventButton *event, PageView * view) {
	view->onButtonPressEvent(widget, event);
	return false;
}

void PageView::handleScrollEvent(GdkEventButton *event) {
	GdkEvent scrollEvent;
	/* with GTK+ 2.17 and later, the entire widget hierarchy is xinput-aware,
	 so the core button event gets discarded and the scroll event never
	 gets processed by the main window. This is arguably a GTK+ bug.
	 We work around it. */
	scrollEvent.scroll.type = GDK_SCROLL;
	scrollEvent.scroll.window = event->window;
	scrollEvent.scroll.send_event = event->send_event;
	scrollEvent.scroll.time = event->time;
	scrollEvent.scroll.x = event->x;
	scrollEvent.scroll.y = event->y;
	scrollEvent.scroll.state = event->state;
	scrollEvent.scroll.device = event->device;
	scrollEvent.scroll.x_root = event->x_root;
	scrollEvent.scroll.y_root = event->y_root;
	if (event->button == 4) {
		scrollEvent.scroll.direction = GDK_SCROLL_UP;
	} else if (event->button == 5) {
		scrollEvent.scroll.direction = GDK_SCROLL_DOWN;
	} else if (event->button == 6) {
		scrollEvent.scroll.direction = GDK_SCROLL_LEFT;
	} else {
		scrollEvent.scroll.direction = GDK_SCROLL_RIGHT;
	}
	gtk_widget_event(gtk_widget_get_parent(xournal->getWidget()), &scrollEvent);

}

bool PageView::searchTextOnPage(const char * text, int * occures, double * top) {
	if (this->search == NULL) {
		if (text == NULL) {
			return true;
		}

		int pNr = page->getPdfPageNr();
		PopplerPage * pdf = NULL;
		if (pNr != -1) {
			pdf = xournal->getControl()->getDocument()->getPdfPage(pNr);
		}
		this->search = new SearchControl(page, pdf);
	}

	bool found = this->search->search(text, occures, top);

	gtk_widget_queue_draw(widget);

	return found;
}

void PageView::addPointToTmpStroke(GdkEventMotion *event) {
	double zoom = xournal->getZoom();
	double x = event->x / zoom;
	double y = event->y / zoom;

	if (tmpStroke->getPointCount() > 0) {
		Point p = tmpStroke->getPoint(tmpStroke->getPointCount() - 1);

		if (hypot(p.x - x, p.y - y) < PIXEL_MOTION_THRESHOLD) {
			return; // not a meaningful motion
		}
	}

	tmpStroke->addPoint(Point(x, y));

	ToolHandler * h = xournal->getControl()->getToolHandler();

	if (h->getToolType() == TOOL_PEN) {
		double presure;
		if (getPressureMultiplier((GdkEvent *) event, presure)) {
			tmpStroke->addWidthValue(tmpStroke->getWidth() * presure);
		}
	}

	drawTmpStroke();
}

bool PageView::getPressureMultiplier(GdkEvent *event, double & presure) {
	double *axes;
	double rawpressure;
	GdkDevice *device;

	if (event->type == GDK_MOTION_NOTIFY) {
		axes = event->motion.axes;
		device = event->motion.device;
	} else {
		axes = event->button.axes;
		device = event->button.device;
	}

	if (device == gdk_device_get_core_pointer() || device->num_axes <= 2) {
		presure = 1.0;
		return false;
	}

	rawpressure = axes[2] / (device->axes[2].max - device->axes[2].min);
	if (!finite(rawpressure)) {
		presure = 1.0;
		return false;
	}

	presure = ((1 - rawpressure) * settings->getWidthMinimumMultiplier() + rawpressure
			* settings->getWidthMaximumMultiplier());
	return true;
}

void PageView::endText() {
	if (!this->textEditor) {
		return;
	}
	Text * txt = this->textEditor->getText();
	Layer * layer = page->getSelectedLayer();
	UndoRedoHandler * undo = xournal->getControl()->getUndoRedoHandler();

	// Text deleted
	if (txt->getText().isEmpty()) {
		// old element
		int pos = layer->indexOf(txt);
		if (pos != -1) {
			DeleteUndoAction * eraseDeleteUndoAction = new DeleteUndoAction(page, this, true);
			layer->removeElement(txt, false);
			eraseDeleteUndoAction->addElement(layer, txt, pos);
			undo->addUndoAction(eraseDeleteUndoAction);
			this->repaint();
		}
	} else {
		// new element
		if (layer->indexOf(txt) == -1) {
			undo->addUndoActionBefore(new InsertUndoAction(page, layer, txt, this),
					this->textEditor->getFirstUndoAction());
			layer->addElement(txt);
			this->textEditor->textCopyed();
			this->repaint();
		}
	}

	delete this->textEditor;
	this->textEditor = NULL;
	repaint();
}

void PageView::startText(double x, double y) {
	if (this->textEditor == NULL) {
		// Is there already a textfield?
		ListIterator<Element *> eit = page->getSelectedLayer()->elementIterator();

		Text * text = NULL;

		while (eit.hasNext()) {
			Element * e = eit.next();

			if (e->getType() == ELEMENT_TEXT) {
				GdkRectangle matchRect = { x - 10, y - 10, 20, 20 };
				if (e->intersectsArea(&matchRect)) {
					text = (Text *) e;
					break;
				}
			}
		}

		bool ownText = false;
		if (text == NULL) {
			ToolHandler * h = xournal->getControl()->getToolHandler();
			ownText = true;
			text = new Text();
			text->setX(x);
			text->setY(y);
			text->setColor(h->getColor());
			// TODO: add font handling
			//			text->setFont();
		}

		this->textEditor = new TextEditor(this, text, ownText);
		if (!ownText) {
			this->textEditor->mousePressed(x - text->getX(), y - text->getY());
		}

		repaint();
	} else {
		Text * text = this->textEditor->getText();
		GdkRectangle matchRect = { x - 10, y - 10, 20, 20 };
		if (!text->intersectsArea(&matchRect)) {
			endText();
		} else {
			this->textEditor->mousePressed(x - text->getX(), y - text->getY());
		}
	}
}

void PageView::selectObjectOn(double x, double y) {
	int selected = page->getSelectedLayerId();
	GdkRectangle matchRect = { x - 10, y - 10, 20, 20 };

	Stroke * strokeMatch = NULL;
	double gap = 1000000000;

	Element * elementMatch = NULL;

	// clear old selection anyway
	clearSelection();

	ListIterator<Layer*> it = page->layerIterator();
	while (it.hasNext() && selected) {
		Layer * l = it.next();

		ListIterator<Element *> eit = l->elementIterator();
		while (eit.hasNext()) {
			Element * e = eit.next();
			if (e->intersectsArea(&matchRect)) {
				if (e->getType() == ELEMENT_STROKE) {
					Stroke * s = (Stroke *) e;
					double tmpGap = 0;
					if (s->intersects(x, y, 20, &tmpGap)) {
						if (gap > tmpGap) {
							gap = tmpGap;
							strokeMatch = s;
						}
					} else {
						printf("intersects == false\n");
					}
				} else {
					elementMatch = e;
				}
			}
		}

		selected--;
	}

	if (strokeMatch) {
		elementMatch = strokeMatch;
	}

	if (elementMatch) {
		this->selection = new EditSelection(elementMatch, this, page);
		gtk_widget_queue_draw(this->widget);
	}
}

/**
 * Handle eraser event: Delete Stroke and Standard, Whiteout is not handled here
 */
void PageView::doErase(double x, double y) {
	ListIterator<Layer*> it = page->layerIterator();

	int selected = page->getSelectedLayerId();
	ToolHandler * h = xournal->getControl()->getToolHandler();

	double halfEraserSize = h->getThikness();
	GdkRectangle eraserRect = { x - halfEraserSize, y - halfEraserSize, halfEraserSize * 2, halfEraserSize * 2 };

	while (it.hasNext() && selected) {
		Layer * l = it.next();

		ListIterator<Element *> eit = l->elementIterator();
		while (eit.hasNext()) {
			Element * e = eit.next();
			if (e->intersectsArea(&eraserRect)) {
				if (e->getType() == ELEMENT_STROKE) {
					Stroke * s = (Stroke *) e;
					if (s->intersects(x, y, halfEraserSize)) {
						if (h->getEraserType() == ERASER_TYPE_DELETE_STROKE) {
							int pos = l->removeElement(e, false);
							if (pos == -1) {
								continue;
							}
							repaint();

							if (!eraseDeleteUndoAction) {
								UndoRedoHandler * undo = xournal->getControl()->getUndoRedoHandler();
								eraseDeleteUndoAction = new DeleteUndoAction(page, this, true);
								undo->addUndoAction(eraseDeleteUndoAction);
							}

							eraseDeleteUndoAction->addElement(l, e, pos);
						} else { // Default
							int pos = l->indexOf(e);
							if (pos == -1) {
								continue;
							}

							if (eraseUndoAction == NULL) {
								UndoRedoHandler * undo = xournal->getControl()->getUndoRedoHandler();
								eraseUndoAction = new EraseUndoAction(page, this);
								undo->addUndoAction(eraseUndoAction);
							}

							if (!s->isCopyed()) {
								Stroke * copy = s->clone();
								eraseUndoAction->addOriginal(l, s, pos);
								eraseUndoAction->addEdited(l, copy, pos);
								copy->setCopyed(true);

								// Because of undo / redo handling:
								// Remove the original and add the copy
								// if we undo this we need the original on the layer, els it can not be identified
								int spos = l->removeElement(s, false);
								l->insertElement(copy, spos);
								s = copy;
							}

							Stroke * part = s->splitOnLastIntersects();

							if (part) {
								l->insertElement(part, pos);
								eraseUndoAction->addEdited(l, part, pos);
								part->setCopyed(true);
							}

							repaint();
						}
					}
				}
			}
		}

		selected--;
	}
}

void PageView::onButtonPressEvent(GtkWidget *widget, GdkEventButton *event) {
#ifdef INPUT_DEBUG
	/**
	 * true: Core event, false: XInput event
	 */
	gboolean isCore = (event->device == gdk_device_get_core_pointer());

	printf("DEBUG: ButtonPress (%s) (x,y)=(%.2f,%.2f), button %d, modifier %x, isCore %i\n", event->device->name,
			event->xScreen, event->yScreen, event->button, event->state, isCore);
#endif

	xournal->resetFocus();

	if (event->type != GDK_BUTTON_PRESS) {
		return;
	}

	if (!this->selected) {
		xournal->getControl()->firePageSelected(this->page);
	}

	if (event->button > 3) { // scroll wheel events! don't paint...
		handleScrollEvent(event);
		return;
	}
	if ((event->state & (GDK_CONTROL_MASK | GDK_MOD1_MASK)) != 0) {
		return;
	}

	// Change the tool depending on the key or device

	ToolHandler * h = xournal->getControl()->getToolHandler();
	ButtonConfig * cfg = NULL;
	ButtonConfig * cfgTouch = settings->getTouchButtonConfig();
	if (event->button == 2) { // Middle Button
		cfg = settings->getMiddleButtonConfig();
	} else if (event->button == 3) { // Right Button
		cfg = settings->getRightButtonConfig();
	} else if (event->device->source == GDK_SOURCE_ERASER) {
		cfg = settings->getEraserButtonConfig();
	} else if (cfgTouch->device == event->device->name) {
		cfg = cfgTouch;

		// If an action is defined we do it, even if it's a drawing action...
		if (cfg->disableDrawing && cfg->action == TOOL_NONE) {
			ToolType tool = h->getToolType();
			if (tool == TOOL_PEN || tool == TOOL_ERASER || tool == TOOL_HILIGHTER) {
				printf("ignore touchscreen for drawing!\n");
				return;
			}
		}
	}

	if (cfg && cfg->action != TOOL_NONE) {
		h->copyCurrentConfig();
		h->selectTool(cfg->action);

		ToolType type = cfg->action;

		if (type == TOOL_PEN || type == TOOL_HILIGHTER) {
			h->setRuler(cfg->rouler);
			h->setShapeRecognizer(cfg->shapeRecognizer);
			if (cfg->size != TOOL_SIZE_NONE) {
				h->setSize(cfg->size);
			}
		}

		if (type == TOOL_PEN || type == TOOL_HILIGHTER || type == TOOL_TEXT) {
			h->setColor(cfg->color);
		}

		if (type == TOOL_ERASER && cfg->eraserMode != ERASER_TYPE_NONE) {
			xournal->getControl()->setEraserType(cfg->eraserMode);
		}

	}

	double x;
	double y;

	if (!gdk_event_get_coords((GdkEvent *) event, &x, &y)) {
		return;
	}

	if ((x < 0 || y < 0) && !extendedWarningDisplayd && settings->isXinputEnabled()) {

		GtkWidget
				* dialog =
						gtk_message_dialog_new(
								(GtkWindow *) *xournal->getControl()->getWindow(),
								GTK_DIALOG_DESTROY_WITH_PARENT,
								GTK_MESSAGE_ERROR,
								GTK_BUTTONS_NONE,
								_("There was a wrong input event, input is not working.\nDo you want to disable \"Extended Input\"?"));

		gtk_dialog_add_button(GTK_DIALOG(dialog), "Disable \"Extended Input\"", 1);
		gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel", 2);

		extendedWarningDisplayd = true;

		if (gtk_dialog_run(GTK_DIALOG(dialog)) == 1) {
			settings->setXinputEnabled(false);
			xournal->updateXEvents();
		}
		gtk_widget_destroy(dialog);
		return;
	}

	double zoom = xournal->getZoom();
	x /= zoom;
	y /= zoom;

	if (h->getToolType() == TOOL_PEN) {
		if (tmpStroke == NULL) {
			currentInputDevice = event->device;
			tmpStroke = new Stroke();
			tmpStroke->setWidth(h->getThikness());
			tmpStroke->setColor(h->getColor());
			tmpStroke->setToolType(STROKE_TOOL_PEN);
			tmpStroke->addPoint(Point(x, y));
		}
	} else if (h->getToolType() == TOOL_HAND) {
		this->lastMousePositionX = 0;
		this->lastMousePositionY = 0;
		this->inScrolling = true;
		gdk_event_get_coords((GdkEvent *) event, &this->lastMousePositionX, &this->lastMousePositionY);
	} else if (h->getToolType() == TOOL_HILIGHTER) {
		if (tmpStroke == NULL) {
			currentInputDevice = event->device;
			tmpStroke = new Stroke();
			tmpStroke->setWidth(h->getThikness());
			tmpStroke->setColor(h->getColor());
			tmpStroke->setToolType(STROKE_TOOL_HIGHLIGHTER);
			tmpStroke->addPoint(Point(x, y));
		}
	} else if (h->getToolType() == TOOL_ERASER) {
		if (h->getEraserType() == ERASER_TYPE_WHITEOUT) {
			currentInputDevice = event->device;
			tmpStroke = new Stroke();
			tmpStroke->setWidth(h->getThikness());
			tmpStroke->setColor(0xffffff); // White
			tmpStroke->setToolType(STROKE_TOOL_ERASER);
			tmpStroke->addPoint(Point(x, y));
		} else {
			doErase(x, y);
			this->inEraser = true;
		}
	} else if (h->getToolType() == TOOL_VERTICAL_SPACE) {
		//		start_vertspace((GdkEvent *) event);


		//
		//	// if this can be a selection move or resize, then it takes precedence over anything else
		//	if (start_resizesel((GdkEvent *) event))
		//		return FALSE;
		//	if (start_movesel((GdkEvent *) event))
		//		return FALSE;

	} else if (h->getToolType() == TOOL_SELECT_RECT || h->getToolType() == TOOL_SELECT_REGION) {
		if (this->selection) {
			CursorSelectionType selType = this->selection->getSelectionTypeForPos(event->x, event->y, zoom);
			if (selType) {
				this->selection->setEditMode(selType, event->x / zoom, event->y / zoom);
				return;
			} else {
				clearSelection();
			}
		}
		if (h->getToolType() == TOOL_SELECT_RECT) {
			if (this->selectionEdit) {
				delete this->selectionEdit;
				this->selectionEdit = NULL;
			}
			this->selectionEdit = new RectSelection(x, y, this);
		} else if (h->getToolType() == TOOL_SELECT_REGION) {
			if (this->selectionEdit) {
				delete this->selectionEdit;
				this->selectionEdit = NULL;
			}
			this->selectionEdit = new RegionSelect(x, y, this);
		}
	} else if (h->getToolType() == TOOL_SELECT_OBJECT) {
		selectObjectOn(x, y);
	} else if (h->getToolType() == TOOL_TEXT) {
		startText(x, y);
	} else if (h->getToolType() == TOOL_IMAGE) {
		//		insert_image((GdkEvent *) event, NULL);
	}

	Cursor * cursor = xournal->getControl()->getCursor();
	cursor->setMouseDown(true);
}

void PageView::clearSelection() {
	delete this->selection;
	this->selection = NULL;
}

void PageView::redrawDocumentRegion(double x1, double y1, double x2, double y2) {
	double zoom = xournal->getZoom();
	gtk_widget_queue_draw_area(this->widget, x1 * zoom - 10, y1 * zoom - 10, (x2 - x1) * zoom + 20, (y2 - y1) * zoom
			+ 20);
}

GdkColor PageView::getSelectionColor() {
	return widget->style->base[GTK_STATE_SELECTED];
}

//gboolean
//on_canvas_enter_notify_event           (GtkWidget       *widget,
//                                        GdkEventCrossing *event,
//                                        gpointer         user_data)
//{
//  GList *dev_list;
//  GdkDevice *dev;
//
//#ifdef INPUT_DEBUG
//  printf("DEBUG: enter notify\n");
//#endif
//    /* re-enable input devices after they've been emergency-disabled
//       by leave_notify */
//  if (!gtk_check_version(2, 17, 0)) {
//    gdk_flush();
//    gdk_error_trap_push();
//    for (dev_list = gdk_devices_list(); dev_list != NULL; dev_list = dev_list->next) {
//      dev = GDK_DEVICE(dev_list->data);
//      gdk_device_set_mode(dev, GDK_MODE_SCREEN);
//    }
//    ui.is_corestroke = ui.saved_is_corestroke;
//    gdk_flush();
//    gdk_error_trap_pop();
//  }
//  return FALSE;
//}
//
//gboolean
//on_canvas_leave_notify_event           (GtkWidget       *widget,
//                                        GdkEventCrossing *event,
//                                        gpointer         user_data)
//{
//  GList *dev_list;
//  GdkDevice *dev;
//
//#ifdef INPUT_DEBUG
//  printf("DEBUG: leave notify (mode=%d, details=%d)\n", event->mode, event->detail);
//#endif
//    /* emergency disable XInput to avoid segfaults (GTK+ 2.17) or
//       interface non-responsiveness (GTK+ 2.18) */
//  if (!gtk_check_version(2, 17, 0)) {
//    gdk_flush();
//    gdk_error_trap_push();
//    for (dev_list = gdk_devices_list(); dev_list != NULL; dev_list = dev_list->next) {
//      dev = GDK_DEVICE(dev_list->data);
//      gdk_device_set_mode(dev, GDK_MODE_DISABLED);
//    }
//    ui.saved_is_corestroke = ui.is_corestroke;
//    ui.is_corestroke = TRUE;
//    gdk_flush();
//    gdk_error_trap_pop();
//  }
//  return FALSE;
//}


gboolean PageView::onMotionNotifyEventCallback(GtkWidget *widget, GdkEventMotion *event, PageView * view) {
	return view->onMotionNotifyEvent(widget, event);
}

gboolean PageView::onMotionNotifyEvent(GtkWidget *widget, GdkEventMotion *event) {
	bool is_core = (event->device == gdk_device_get_core_pointer());
#ifdef INPUT_DEBUG
	printf("DEBUG: MotionNotify (%s) (x,y)=(%.2f,%.2f), modifier %x\n", is_core ? "core" : "xinput", event->xScreen,
			event->yScreen, event->state);
#endif

	double zoom = xournal->getZoom();
	double x = event->x / zoom;
	double y = event->y / zoom;
	ToolHandler * h = xournal->getControl()->getToolHandler();
	if (h->getToolType() == TOOL_HAND) {
		if (this->inScrolling) {
			doScroll(event);
		}
	} else if (h->getToolType() == TOOL_ERASER && h->getEraserType() != ERASER_TYPE_WHITEOUT && this->inEraser) {
		doErase(x, y);
	} else {
		if (tmpStroke != NULL && currentInputDevice == event->device) {
			addPointToTmpStroke(event);
		} else if (this->selectionEdit) {
			this->selectionEdit->currentPos(x, y);
		} else if (this->selection) {
			if (this->selection->getEditMode()) {
				this->selection->move(x, y, this, xournal);
			} else {
				Cursor * cursor = xournal->getControl()->getCursor();

				CursorSelectionType selType = this->selection->getSelectionTypeForPos(event->x, event->y, zoom);
				cursor->setMouseSelectionType(selType);
			}
		} else if (this->textEditor) {
			Cursor * cursor = getXournal()->getControl()->getCursor();
			cursor->setInvisible(false);

			Text * text = this->textEditor->getText();
			this->textEditor->mouseMoved(x - text->getX(), y - text->getY());
		}
	}

	return false;
}

void PageView::doScroll(GdkEventMotion *event) {
	double x = 0;
	double y = 0;

	gdk_event_get_coords((GdkEvent *) event, &x, &y);

	if (ABS(this->lastMousePositionX-x) < 3 && ABS(this->lastMousePositionY-y) < 3) {
		return;
	}

	xournal->getControl()->scrollRelative(this->lastMousePositionX - x, this->lastMousePositionY - y);

	gint ix = 0;
	gint iy = 0;
	gtk_widget_get_pointer(widget, &ix, &iy);

	this->lastMousePositionX = ix;
	this->lastMousePositionY = iy;
}

bool PageView::onButtonReleaseEventCallback(GtkWidget *widget, GdkEventButton *event, PageView * view) {
	return view->onButtonReleaseEvent(widget, event);
}

// TODO: if you rotate the screen the events dont match the pointer coordinates, this is may a GTK Bug, but has to be fixed!

bool PageView::onButtonReleaseEvent(GtkWidget *widget, GdkEventButton *event) {
#ifdef INPUT_DEBUG
	gboolean isCore = (event->device == gdk_device_get_core_pointer());
	printf("DEBUG: ButtonRelease (%s) (x,y)=(%.2f,%.2f), button %d, modifier %x, isCore %i\n", event->device->name,
			event->xScreen, event->yScreen, event->button, event->state, isCore);
#endif

	//	if (event->button != ui.which_mouse_button && event->button != ui.which_unswitch_button)
	//		return FALSE;
	//
	if (tmpStroke) {
		// Backward compatibility and also easyer to handle for me;-)
		// I cannont draw a line with one point, to draw a visible line I need two points,
		// twice the same Point is also OK
		if (tmpStroke->getPointCount() == 1) {
			ArrayIterator<Point> it = tmpStroke->pointIterator();
			if (it.hasNext()) {
				tmpStroke->addPoint(it.next());
			}
			// No Presure sensitivity
			tmpStroke->clearWidths();
		}

		tmpStroke->freeUnusedPointItems();
		tmpStroke->freeUnusedWidthItems();

		if (page->getSelectedLayerId() < 1) {
			// This creates a layer if none exists
			page->getSelectedLayer();
			page->setSelectedLayerId(1);
			xournal->getControl()->getWindow()->updateLayerCombobox();
			repaint();
		}

		Layer * layer = page->getSelectedLayer();

		UndoRedoHandler * undo = xournal->getControl()->getUndoRedoHandler();

		undo->addUndoAction(new InsertUndoAction(page, layer, tmpStroke, this));

		ToolHandler * h = xournal->getControl()->getToolHandler();
		if (h->isShapeRecognizer()) {
			ShapeRecognizer reco;

			Stroke * s = reco.recognizePatterns(tmpStroke);

			if (s != NULL) {
				UndoRedoHandler * undo = xournal->getControl()->getUndoRedoHandler();

				undo->addUndoAction(new RecognizerUndoAction(page, this, layer, tmpStroke, s));

				layer->addElement(s);

				repaint();
			} else {
				layer->addElement(tmpStroke);
				repaint(tmpStroke->getX(), tmpStroke->getY(), tmpStroke->getElementWidth(),
						tmpStroke->getElementHeight());
			}
		} else {
			layer->addElement(tmpStroke);
			repaint(tmpStroke->getX(), tmpStroke->getY(), tmpStroke->getElementWidth(), tmpStroke->getElementHeight());
		}

		tmpStroke = NULL;

		if (currentInputDevice == event->device) {
			currentInputDevice = NULL;
		}
	}

	ToolHandler * h = xournal->getControl()->getToolHandler();
	h->restoreLastConfig();

	Cursor * cursor = xournal->getControl()->getCursor();
	cursor->setMouseDown(false);

	this->inScrolling = false;
	this->inEraser = false;
	this->eraseDeleteUndoAction = false;

	if (eraseUndoAction) {
		eraseUndoAction->cleanup();
		ListIterator<Layer*> pit = page->layerIterator();
		while (pit.hasNext()) {
			Layer * l = pit.next();
			ListIterator<Element *> lit = l->elementIterator();
			while (lit.hasNext()) {
				Element * e = lit.next();
				if (e->getType() == ELEMENT_STROKE) {
					Stroke * s = (Stroke *) e;
					s->setCopyed(false);
					s->freeUnusedPointItems();
					s->freeUnusedWidthItems();
				}
			}
		}

		eraseUndoAction = NULL;
	}

	if (this->selectionEdit) {
		if (this->selectionEdit->finnalize(this->page)) {
			delete selection;
			selection = new EditSelection(this->selectionEdit, this);
		}
		delete this->selectionEdit;
		this->selectionEdit = NULL;
	} else if (this->selection) {
		this->selection->finalizeEditing();
	} else if (this->textEditor) {
		this->textEditor->mouseReleased();
	}

	return false;
}

bool PageView::onKeyPressEvent(GdkEventKey *event) {
	// Esc leaves text edition
	if (event->keyval == GDK_Escape) {
		if (this->textEditor) {
			endText();
			return true;
		} else {
			return false;
		}
	}

	if (this->textEditor && this->textEditor->onKeyPressEvent(event)) {
		return true;
	}

	return false;
}

bool PageView::onKeyReleaseEvent(GdkEventKey *event) {
	if (this->textEditor && this->textEditor->onKeyReleaseEvent(event)) {
		return true;
	}

	return false;
}

GtkWidget * PageView::getWidget() {
	return widget;
}

XournalWidget * PageView::getXournal() {
	return xournal;
}

double PageView::getHeight() {
	return page->getHeight();
}

double PageView::getWidth() {
	return page->getWidth();
}

int PageView::getDisplayWidth() {
	return page->getWidth() * xournal->getZoom();
}

int PageView::getDisplayHeight() {
	return page->getHeight() * xournal->getZoom();
}

gboolean PageView::exposeEventCallback(GtkWidget *widget, GdkEventExpose *event, PageView * page) {
	return page->paintPage(widget, event);
}

XojPage * PageView::getPage() {
	return page;
}

void PageView::repaint() {
	deleteViewBuffer();
	gtk_widget_queue_draw(widget);
}

void PageView::repaint(int x, int y, int width, int heigth) {
	deleteViewBuffer();
	double zoom = xournal->getZoom();
	gtk_widget_queue_draw_area(widget, x * zoom, y * zoom, width * zoom, heigth * zoom);
}

void PageView::updateSize() {
	gtk_widget_set_size_request(widget, getDisplayWidth(), getDisplayHeight());
}

void PageView::setSelected(bool selected) {
	this->selected = selected;

	GtkWidget * parent = gtk_widget_get_parent(this->widget);

	GtkAllocation alloc = { 0 };
	gtk_widget_get_allocation(parent, &alloc);
	gtk_widget_queue_draw(parent);

	if (selected) {
		gtk_widget_grab_focus(widget);
	}
}

void PageView::firstPaint() {
	if (!GDK_IS_WINDOW(this->widget->window)) {
		return;
	}

	this->firstPainted = true;

	gdk_window_set_background(this->widget->window, &this->widget->style->white);
	gtk_widget_queue_draw(this->widget);
}

bool PageView::repaintCallback(PageView * view) {
	view->idleRepaintId = 0;

	view->deleteViewBuffer();
	view->paintPage(view->widget, NULL);
	return false;
}

void PageView::repaintLater() {
	if (idleRepaintId) {
		return;
	}
	idleRepaintId = g_idle_add_full(G_PRIORITY_LOW, (GSourceFunc) &repaintCallback, this, NULL);
}

void PageView::drawTmpStroke() {
	if (tmpStroke) {
		cairo_t *cr;
		cr = gdk_cairo_create(widget->window);

		cairo_scale(cr, xournal->getZoom(), xournal->getZoom());

		view->drawStroke(cr, tmpStroke, tmpStrokeDrawElem);
		tmpStrokeDrawElem = tmpStroke->getPointCount() - 1;
		cairo_destroy(cr);
	}
}

bool PageView::isSelected() {
	return selected;
}

void PageView::cut() {
	if (this->textEditor) {
		this->textEditor->cutToClipboard();
	}
}

void PageView::copy() {
	if (this->textEditor) {
		this->textEditor->copyToCliboard();
	}
}

void PageView::paste() {
	if (this->textEditor) {
		this->textEditor->pasteFromClipboard();
	}
}

void PageView::actionDelete() {
	if (this->textEditor) {
		this->textEditor->deleteFromCursor(GTK_DELETE_CHARS, 1);
	}
}

bool PageView::paintPage(GtkWidget *widget, GdkEventExpose *event) {
	if (!firstPainted) {
		firstPaint();
		return true;
	}

	cairo_t *cr;
	cr = gdk_cairo_create(widget->window);

	GtkAllocation alloc;
	gtk_widget_get_allocation(widget, &alloc);

	if (crBuffer == NULL) {
		crBuffer = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, alloc.width, alloc.height);

		cairo_t *cr2;
		cr2 = cairo_create(crBuffer);

		this->tmpStrokeDrawElem = 0;

		cairo_scale(cr2, xournal->getZoom(), xournal->getZoom());

		PopplerPage * popplerPage = NULL;

		if (page->getBackgroundType() == BACKGROUND_TYPE_PDF) {
			int pgNo = page->getPdfPageNr();
			popplerPage = xournal->getDocument()->getPdfPage(pgNo);
		}

		view->drawPage(page, popplerPage, cr2);

		if (popplerPage == NULL) {
			//TODO: add request for page
		}

		cairo_destroy(cr2);
	}

	double width = cairo_image_surface_get_width(crBuffer);
	if (width != alloc.width) {
		double scale = ((double) alloc.width) / ((double) width);

		// Scale current image to fit the zoom level
		cairo_matrix_t defaultMatrix = { 0 };
		cairo_get_matrix(cr, &defaultMatrix);

		cairo_scale(cr, scale, scale);
		cairo_set_source_surface(cr, crBuffer, 0, 0);

		cairo_set_matrix(cr, &defaultMatrix);

		repaintLater();
	} else {
		cairo_set_source_surface(cr, crBuffer, 0, 0);
	}

	cairo_paint(cr);

	cairo_matrix_t defaultMatrix = { 0 };
	cairo_get_matrix(cr, &defaultMatrix);
	cairo_scale(cr, xournal->getZoom(), xournal->getZoom());

	if (this->textEditor) {
		this->textEditor->paint(cr, event, xournal->getZoom());
	}
	if (this->selectionEdit) {
		this->selectionEdit->paint(cr, event, xournal->getZoom());
	}
	if (this->selection) {
		this->selection->paint(cr, event, xournal->getZoom());
	}
	if (this->search) {
		this->search->paint(cr, event, xournal->getZoom(), getSelectionColor());
	}
	if (this->tmpStroke) {
		view->drawStroke(cr, this->tmpStroke);
	}

	cairo_set_matrix(cr, &defaultMatrix);

	cairo_destroy(cr);
	return true;
}

