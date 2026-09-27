[@@@warning "+48"]
let f ?(x = 1) () = x
let g (h : unit -> int) = h ()
let y = g f
