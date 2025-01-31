# ---------------------------------------------
# Formatting options
# ---------------------------------------------
line_width = 120             # Adjust line width for readability
tab_size = 4                 # Set indentation size
use_tabchars = False         # Use spaces instead of tabs
max_subgroups_hwrap = 2      # Control wrapping of nested commands
separate_ctrl_name_with_space = False  # Space after control command names
separate_fn_name_with_space = False   # No space after function/macro names

# ---------------------------------------------
# Command-specific formatting
# ---------------------------------------------
keyword_case = 'upper'       # Keep CMake keywords lowercase
command_case = 'lower'       # Keep CMake commands lowercase
always_wrap = []             # List of commands that should always be wrapped
enable_sort = True           # Sort lists where possible
autosort = True              # Automatically sort variable assignments

# ---------------------------------------------
# Argument grouping & wrapping
# ---------------------------------------------
dangle_parens = False        # Avoid dangling parentheses on separate lines
max_pargs_hwrap = 4          # Max args before wrapping function calls
min_prefix_chars = 4         # Minimum prefix length before wrapping
max_prefix_chars = 10        # Maximum prefix length before wrapping
max_lines_hwrap = 2          # Limit wrapping in nested function calls

# ---------------------------------------------
# Miscellaneous
# ---------------------------------------------
line_ending = 'unix'         # Use Unix-style line endings (`\n`)
