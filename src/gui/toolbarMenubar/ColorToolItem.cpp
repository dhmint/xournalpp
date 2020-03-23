#include "ColorToolItem.h"

#include "model/ToolbarColorNames.h"
#include "gui/toolbarMenubar/icon/ColorSelectImage.h"

#include <config.h>
#include <i18n.h>
#include <StringUtils.h>
#include <Util.h>

bool ColorToolItem::inUpdate = false;

ColorToolItem::ColorToolItem(ActionHandler* handler, ToolHandler* toolHandler, GtkWindow* parent, unsigned int color, bool selektor)
 : AbstractToolItem("", handler, selektor ? ACTION_SELECT_COLOR_CUSTOM : ACTION_SELECT_COLOR),
   color(color),
   toolHandler(toolHandler),
   parent(parent)
{
	XOJ_INIT_TYPE(ColorToolItem);

	this->group = GROUP_COLOR;

	updateName();
}

ColorToolItem::~ColorToolItem()
{
	XOJ_CHECK_TYPE(ColorToolItem);

	freeIcons();

	XOJ_RELEASE_TYPE(ColorToolItem);
}

/**
 * Free the allocated icons
 */
void ColorToolItem::freeIcons()
{
	XOJ_CHECK_TYPE(ColorToolItem);

	delete this->icon;
	this->icon = NULL;
}

bool ColorToolItem::isSelector()
{
	XOJ_CHECK_TYPE(ColorToolItem);

	return this->action == ACTION_SELECT_COLOR_CUSTOM;
}

void ColorToolItem::updateName()
{
	XOJ_CHECK_TYPE(ColorToolItem);

	if (this->action == ACTION_SELECT_COLOR_CUSTOM)
	{
		this->name = _("Select color");
	}
	else
	{
		this->name = ToolbarColorNames::getInstance().getColorName(this->color);
	}
}

void ColorToolItem::actionSelected(ActionGroup group, ActionType action)
{
	XOJ_CHECK_TYPE(ColorToolItem);

	inUpdate = true;
	if (this->group == group && this->item)
	{
		if (isSelector())
		{
			gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(this->item), isSelector());
		}
		enableColor(toolHandler->getColor());
	}
	inUpdate = false;
}

void ColorToolItem::enableColor(unsigned int color)
{
	XOJ_CHECK_TYPE(ColorToolItem);

	if (isSelector())
	{
		if (this->icon)
		{
			this->icon->setColor(color);
		}

		this->color = color;
		if (GTK_IS_TOGGLE_BUTTON(this->item))
		{
			gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(this->item), false);
		}
	}
	else
	{
		if (this->item)
		{
			gtk_toggle_tool_button_set_active(GTK_TOGGLE_TOOL_BUTTON(this->item), this->color == color);
		}
	}
}

unsigned int ColorToolItem::getColor() const
{
	XOJ_CHECK_TYPE(ColorToolItem);

	return this->color;
}

string ColorToolItem::getId()
{
	XOJ_CHECK_TYPE(ColorToolItem);

	if (isSelector())
	{
		return "COLOR_SELECT";
	}

	char buffer[64];
	sprintf(buffer, "COLOR(0x%06x)", this->color);
	string id = buffer;

	return id;
}

/**
 * Show colochooser to select a custom color
 */
void ColorToolItem::showColorchooser()
{
	GtkWidget* dialog = gtk_color_chooser_dialog_new(_("Select color"), parent);
	gtk_color_chooser_set_use_alpha(GTK_COLOR_CHOOSER(dialog), false);

	int response = gtk_dialog_run(GTK_DIALOG(dialog));
	if (response == GTK_RESPONSE_OK)
	{
		GdkRGBA color;
		gtk_color_chooser_get_rgba(GTK_COLOR_CHOOSER(dialog), &color);
		this->color = (((int)(color.red * 255)) & 0xff) << 16 |
				(((int)(color.green * 255)) & 0xff) << 8 |
				(((int)(color.blue * 255)) & 0xff);
	}

	gtk_widget_destroy(dialog);
}

/**
 * Enable / Disable the tool item
 */
void ColorToolItem::enable(bool enabled)
{
	if (!enabled && toolHandler->getToolType() == TOOL_ERASER)
	{
		if (this->icon)
		{
			icon->setState(COLOR_ICON_STATE_PEN);
		}
		AbstractToolItem::enable(true);
		switchToPen = true;
		return;
	}

	switchToPen = false;
	AbstractToolItem::enable(enabled);
	if (this->icon)
	{
		if (enabled)
		{
			icon->setState(COLOR_ICON_STATE_ENABLED);
		} else
		{
			icon->setState(COLOR_ICON_STATE_DISABLED);
		}
	}
}

void ColorToolItem::activated(GdkEvent* event, GtkMenuItem* menuitem, GtkToolButton* toolbutton)
{
	XOJ_CHECK_TYPE(ColorToolItem);

	if (switchToPen)
	{
		toolHandler->selectTool(TOOL_PEN, true);
	}

	if (inUpdate)
	{
		return;
	}
	inUpdate = true;

	if (isSelector())
	{
		showColorchooser();
	}

	toolHandler->setColor(this->color, true);

	inUpdate = false;
}

GtkToolItem* ColorToolItem::newItem()
{
	XOJ_CHECK_TYPE(ColorToolItem);

	this->icon = new ColorSelectImage(this->color, !isSelector());

	GtkToolItem* it = gtk_toggle_tool_button_new();

	const gchar* name = this->name.c_str();
	gtk_tool_item_set_tooltip_text(GTK_TOOL_ITEM(it), name);
	gtk_tool_button_set_label(GTK_TOOL_BUTTON(it), name);

	gtk_tool_button_set_icon_widget(GTK_TOOL_BUTTON(it), this->icon->getWidget());

	return it;
}

string ColorToolItem::getToolDisplayName()
{
	XOJ_CHECK_TYPE(ColorToolItem);

	return this->name;
}

GtkWidget* ColorToolItem::getNewToolIcon()
{
	XOJ_CHECK_TYPE(ColorToolItem);

	return ColorSelectImage::newColorIcon(this->color, 16, !isSelector());
}
