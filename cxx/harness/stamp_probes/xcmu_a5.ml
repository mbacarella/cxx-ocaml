(* copy_commu: the unknown arrow reaches the outer scheme through an instance *)
let init n f =
  let rec aux i f = if i = n then [] else f i :: aux (i + 1) f in
  aux 0 f
