type z = int
let g () = let module Int = struct end in ()
module S = Set.Make (Int)
