let f g = match g () with x -> x | exception Not_found -> 0
let h g = match g () with Some x -> x | None -> 1 | exception (Failure _) -> 2
