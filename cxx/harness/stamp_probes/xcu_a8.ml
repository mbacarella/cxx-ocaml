module type S = Hashtbl.HashedType
module H = Hashtbl.Make (struct
  type t = int
  let equal = ( = )
  let hash = Hashtbl.hash
end)
let h : int H.t = H.create 4
