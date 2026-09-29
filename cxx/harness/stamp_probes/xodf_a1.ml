let g () = print_string "a"
let f ?(x : unit -> unit = g) () = x
