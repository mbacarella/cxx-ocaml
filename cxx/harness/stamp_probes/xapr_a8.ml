let g () = print_string "a"
let h () = ()
let t = [| Some h; Some g; None |]
