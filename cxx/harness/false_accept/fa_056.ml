let compose f g x = f (g x) let y = compose String.length string_of_int "a"
