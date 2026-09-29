let g () = print_string "a"
let f ?(x = g) () = x
