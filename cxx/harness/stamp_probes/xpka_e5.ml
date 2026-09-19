type z = int
let g () = let module Set = struct end in ()
let f () = let module S = Set.Make (Int) in S.cardinal S.empty
