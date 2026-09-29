module X = struct
  type t = int
  let equal (a : t) b = a = b
  let hash (x : t) = x
  let seeded_hash (_ : int) (x : t) = x
end
type t = int Hashtbl.Make(X).t
