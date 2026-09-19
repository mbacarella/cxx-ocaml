type z = int
let g () = let module Set = struct let x = 1 end in Set.x
module S = Set.Make (Int)
