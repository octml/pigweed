# Copyright 2021 The Pigweed Authors
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.
"""Help window container class."""

import functools
import inspect
import logging
from pathlib import Path
from typing import Dict, TYPE_CHECKING

from prompt_toolkit.document import Document
from prompt_toolkit.filters import Condition
from prompt_toolkit.key_binding import KeyBindings, KeyPressEvent
from prompt_toolkit.layout import (
    ConditionalContainer,
    DynamicContainer,
    FormattedTextControl,
    HSplit,
    VSplit,
    Window,
    WindowAlign,
)
from prompt_toolkit.layout.dimension import Dimension
from prompt_toolkit.lexers import PygmentsLexer
from prompt_toolkit.widgets import Box, TextArea

from pygments.lexers.markup import RstLexer  # type: ignore
import pw_console.widgets.mouse_handlers

if TYPE_CHECKING:
    from pw_console.console_app import ConsoleApp

_LOG = logging.getLogger(__package__)


def _longest_line_length(text):
    """Return the longest line in the given text."""
    max_line_length = 0
    for line in text.splitlines():
        if len(line) > max_line_length:
            max_line_length = len(line)
    return max_line_length


class HelpWindow(ConditionalContainer):
    """Help window container for displaying keybindings."""

    # pylint: disable=too-many-instance-attributes

    def _create_help_text_area(self, **kwargs):
        help_text_area = TextArea(
            focusable=True,
            focus_on_click=True,
            scrollbar=True,
            style='class:help_window_content',
            **kwargs,
        )

        # Additional keybindings for the text area.
        key_bindings = KeyBindings()

        @key_bindings.add('q')
        @key_bindings.add('f1')
        def _close_window(_event: KeyPressEvent) -> None:
            """Close the current dialog window."""
            self.toggle_display()

        help_text_area.control.key_bindings = key_bindings
        return help_text_area

    def __init__(self,
                 application: 'ConsoleApp',
                 preamble: str = '',
                 additional_help_text: str = '',
                 title: str = '') -> None:
        # Dict containing key = section title and value = list of key bindings.
        self.application: 'ConsoleApp' = application
        self.show_window: bool = False
        self.help_text_sections: Dict[str, Dict] = {}
        self._pane_title: str = title

        # Generated keybinding text
        self.preamble: str = preamble
        self.additional_help_text: str = additional_help_text
        self.help_text: str = ''

        self.max_additional_help_text_width: int = (_longest_line_length(
            self.additional_help_text) if additional_help_text else 0)
        self.max_description_width: int = 0
        self.max_key_list_width: int = 0
        self.max_line_length: int = 0

        self.help_text_area: TextArea = self._create_help_text_area()

        close_mouse_handler = functools.partial(
            pw_console.widgets.mouse_handlers.on_click, self.toggle_display)

        toolbar_padding = 1
        toolbar_title = ' ' * toolbar_padding
        toolbar_title += self.pane_title()

        top_toolbar = VSplit(
            [
                Window(
                    content=FormattedTextControl(
                        # [('', toolbar_title)]
                        functools.partial(pw_console.style.get_pane_indicator,
                                          self, toolbar_title)),
                    align=WindowAlign.LEFT,
                    dont_extend_width=True,
                ),
                Window(
                    content=FormattedTextControl([]),
                    align=WindowAlign.LEFT,
                    dont_extend_width=False,
                ),
                Window(
                    content=FormattedTextControl(
                        pw_console.widgets.checkbox.to_keybind_indicator(
                            'q',
                            'Close',
                            close_mouse_handler,
                            base_style='class:toolbar-button-active')),
                    align=WindowAlign.RIGHT,
                    dont_extend_width=True,
                ),
            ],
            height=1,
            style='class:toolbar_active',
        )

        self.container = HSplit([
            top_toolbar,
            Box(
                body=DynamicContainer(lambda: self.help_text_area),
                padding=Dimension(preferred=1, max=1),
                padding_bottom=0,
                padding_top=0,
                char=' ',
                style='class:frame.border',  # Same style used for Frame.
            ),
        ])

        super().__init__(
            self.container,
            filter=Condition(lambda: self.show_window),
        )

    def pane_title(self):
        return self._pane_title

    def menu_title(self):
        """Return the title to display in the Window menu."""
        return self.pane_title()

    def __pt_container__(self):
        """Return the prompt_toolkit container for displaying this HelpWindow.

        This allows self to be used wherever prompt_toolkit expects a container
        object."""
        return self.container

    def toggle_display(self):
        """Toggle visibility of this help window."""
        # Toggle state variable.
        self.show_window = not self.show_window

        # Set the help window in focus.
        if self.show_window:
            self.application.last_focused_pane = (
                self.application.focused_window())
            self.application.layout.focus(self.help_text_area)
        # Restore original focus.
        else:
            if self.application.last_focused_pane:
                self.application.layout.focus(
                    self.application.last_focused_pane)
            self.application.last_focused_pane = None

    def content_width(self) -> int:
        """Return total width of help window."""
        # Widths of UI elements
        frame_width = 1
        padding_width = 1
        left_side_frame_and_padding_width = frame_width + padding_width
        right_side_frame_and_padding_width = frame_width + padding_width
        scrollbar_padding = 1
        scrollbar_width = 1

        return self.max_line_length + (left_side_frame_and_padding_width +
                                       right_side_frame_and_padding_width +
                                       scrollbar_padding + scrollbar_width)

    def load_user_guide(self):
        rstdoc = Path(__file__).parent / 'docs/user_guide.rst'
        max_line_length = 0
        rst_text = ''
        with rstdoc.open() as rstfile:
            for line in rstfile.readlines():
                if 'https://' not in line and len(line) > max_line_length:
                    max_line_length = len(line)
                rst_text += line
        self.max_line_length = max_line_length

        self.help_text_area = self._create_help_text_area(
            lexer=PygmentsLexer(RstLexer),
            text=rst_text,
        )

    def generate_help_text(self):
        """Generate help text based on added key bindings."""

        template = self.application.get_template('keybind_list.jinja')

        self.help_text = template.render(
            sections=self.help_text_sections,
            max_additional_help_text_width=self.max_additional_help_text_width,
            max_description_width=self.max_description_width,
            max_key_list_width=self.max_key_list_width,
            preamble=self.preamble,
            additional_help_text=self.additional_help_text,
        )

        # Find the longest line in the rendered template.
        self.max_line_length = _longest_line_length(self.help_text)

        # Replace the TextArea content.
        self.help_text_area.buffer.document = Document(text=self.help_text,
                                                       cursor_position=0)

        return self.help_text

    def add_custom_keybinds_help_text(self, section_name, key_bindings: Dict):
        """Add hand written key_bindings."""
        self.help_text_sections[section_name] = key_bindings

    def add_keybind_help_text(self, section_name, key_bindings: KeyBindings):
        """Append formatted key binding text to this help window."""

        # Create a new keybind section, erasing any old section with thesame
        # title.
        self.help_text_sections[section_name] = {}

        # Loop through passed in prompt_toolkit key_bindings.
        for binding in key_bindings.bindings:
            # Skip this keybind if the method name ends in _hidden.
            if binding.handler.__name__.endswith('_hidden'):
                continue

            # Get the key binding description from the function doctstring.
            docstring = binding.handler.__doc__
            if not docstring:
                docstring = ''
            description = inspect.cleandoc(docstring)
            description = description.replace('\n', ' ')

            # Save the length of the description.
            if len(description) > self.max_description_width:
                self.max_description_width = len(description)

            # Get the existing list of keys for this function or make a new one.
            key_list = self.help_text_sections[section_name].get(
                description, list())

            # Save the name of the key e.g. F1, q, ControlQ, ControlUp
            key_name = '-'.join(
                [getattr(key, 'name', str(key)) for key in binding.keys])
            key_name = key_name.replace('Control', 'Ctrl-')
            key_name = key_name.replace('Shift', 'Shift-')
            key_name = key_name.replace('Escape-', 'Alt-')
            key_list.append(key_name)

            key_list_width = len(', '.join(key_list))
            # Save the length of the key list.
            if key_list_width > self.max_key_list_width:
                self.max_key_list_width = key_list_width

            # Update this functions key_list
            self.help_text_sections[section_name][description] = key_list
