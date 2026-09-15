module H = struct
  type t = int
  let equal = (=)
  let hash = Hashtbl.hash
  let seeded_hash = Hashtbl.seeded_hash
end
module A = Hashtbl.MakeSeeded (H)
module B = Ephemeron.K1.MakeSeeded (H)
