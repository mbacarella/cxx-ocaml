let f () = ()
let g () = Gc.major ()
let t = [| ("a", fun () -> f ()); ("b", g) |]
