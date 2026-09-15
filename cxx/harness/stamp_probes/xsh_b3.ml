module H = struct
  type t = int
  let equal = (=)
  let hash = Hashtbl.hash
  let seeded_hash = Hashtbl.seeded_hash
end
module A = Ephemeron.K2.MakeSeeded (H) (H)
