let g = [print_string]
let f ?(x : (string -> unit) list = g) () = List.length x
