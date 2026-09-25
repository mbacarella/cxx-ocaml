let f () = ()
let g () = print_string "a"
let t = [| ("a", fun () -> f ()); ("b", g) |]
