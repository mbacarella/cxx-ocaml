let f x = print_int x; print_newline (); x + 1
let g () = for i = 0 to 3 do print_int i done
let h () = let i = ref 0 in while !i < 3 do incr i done; !i
