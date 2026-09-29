type z = int
module S = Set.Make (Int)
let f () = (module S : Set.S)
