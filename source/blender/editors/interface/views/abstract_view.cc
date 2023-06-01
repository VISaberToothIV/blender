/* SPDX-FileCopyrightText: 2023 Blender Foundation
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edinterface
 */

#include "ED_select_utils.h"

#include "interface_intern.hh"

#include "UI_abstract_view.hh"

namespace blender::ui {

void AbstractView::register_item(AbstractViewItem &item)
{
  /* Actually modifies the item, not the view.  But for the public API it "feels" a bit nicer to
   * have the view base class register the items, rather than setting the view on the item. */
  item.view_ = this;
}

/* ---------------------------------------------------------------------- */
/** \name View Reconstruction
 * \{ */

bool AbstractView::is_reconstructed() const
{
  return is_reconstructed_;
}

void AbstractView::update_from_old(uiBlock &new_block)
{
  uiBlock *old_block = new_block.oldblock;
  if (!old_block) {
    is_reconstructed_ = true;
    return;
  }

  uiViewHandle *old_view_handle = ui_block_view_find_matching_in_old_block(
      &new_block, reinterpret_cast<uiViewHandle *>(this));
  if (old_view_handle == nullptr) {
    /* Initial construction, nothing to update. */
    is_reconstructed_ = true;
    return;
  }

  AbstractView &old_view = reinterpret_cast<AbstractView &>(*old_view_handle);

  /* Update own persistent data. */
  /* Keep the rename buffer persistent while renaming! The rename button uses the buffer's
   * pointer to identify itself over redraws. */
  rename_buffer_ = std::move(old_view.rename_buffer_);
  old_view.rename_buffer_ = nullptr;

  update_children_from_old(old_view);

  /* Finished (re-)constructing the tree. */
  is_reconstructed_ = true;
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Default implementations of virtual functions
 * \{ */

std::unique_ptr<AbstractViewDropTarget> AbstractView::create_drop_target()
{
  /* There's no drop target (and hence no drop support) by default. */
  return nullptr;
}

bool AbstractView::listen(const wmNotifier & /*notifier*/) const
{
  /* Nothing by default. */
  return false;
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Renaming
 * \{ */

bool AbstractView::is_renaming() const
{
  return rename_buffer_ != nullptr;
}

bool AbstractView::begin_renaming()
{
  if (is_renaming()) {
    return false;
  }

  rename_buffer_ = std::make_unique<decltype(rename_buffer_)::element_type>();
  return true;
}

void AbstractView::end_renaming()
{
  BLI_assert(is_renaming());
  rename_buffer_ = nullptr;
}

Span<char> AbstractView::get_rename_buffer() const
{
  return *rename_buffer_;
}
MutableSpan<char> AbstractView::get_rename_buffer()
{
  return *rename_buffer_;
}

std::optional<rcti> AbstractView::get_bounds() const
{
  return bounds_;
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name Selection
 * \{ */

/**
 * If \a action is #SEL_TOGGLE, check the selection if the actual action to apply should be
 * #SEL_DESELECT or #SEL_SELECT and return that.
 */
static SelectAction select_all_refine_action_type(const AbstractView &view, SelectAction action)
{
  if (action != SEL_TOGGLE) {
    return action;
  }

  bool any_selected = false;
  view.foreach_abstract_item([&any_selected](AbstractViewItem &item) {
    if (item.is_selected()) {
      any_selected = true;
    }
  });

  return any_selected ? SEL_DESELECT : SEL_SELECT;
}

static bool view_item_select_from_action(AbstractViewItem &item, const SelectAction action)
{
  switch (action) {
    case SEL_SELECT:
      return item.select();
    case SEL_DESELECT:
      return item.deselect();
    case SEL_INVERT:
      if (item.is_selected()) {
        item.deselect();
      }
      else {
        item.select();
      }
      return true;
    case SEL_TOGGLE:
      BLI_assert_msg(
          false,
          "TOGGLE action should have been refined to be either SELECT or DESELECT at this point");
      break;
  }

  return false;
}

bool view_item_select_from_action(uiViewItemHandle *item_handle, const int /*SelectAction*/ action)
{
  AbstractViewItem &item = reinterpret_cast<AbstractViewItem &>(*item_handle);
  return view_item_select_from_action(item, SelectAction(action));
}

bool view_select_all_from_action(uiViewHandle *view_handle, const int /*SelectAction*/ action)
{
  AbstractView &view = reinterpret_cast<AbstractView &>(*view_handle);
  const SelectAction refined_action = select_all_refine_action_type(view, SelectAction(action));

  bool changed = false;
  view.foreach_abstract_item([&changed, refined_action](AbstractViewItem &item) {
    changed |= view_item_select_from_action(item, refined_action);
  });

  return changed;
}

/** \} */

/* ---------------------------------------------------------------------- */
/** \name General API functions
 * \{ */

std::unique_ptr<DropTargetInterface> view_drop_target(uiViewHandle *view_handle)
{
  AbstractView &view = reinterpret_cast<AbstractView &>(*view_handle);
  return view.create_drop_target();
}

/** \} */

}  // namespace blender::ui
