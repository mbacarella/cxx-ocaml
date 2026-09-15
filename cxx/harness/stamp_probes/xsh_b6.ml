module H = struct
  type t = int
  let equal = (=)
  let hash = Hashtbl.hash
  let seeded_hash = Hashtbl.seeded_hash
end
module A = Hashtbl.Make (H)
module G (Y : Hashtbl.SeededHashedType) = struct type u = Y.t end
