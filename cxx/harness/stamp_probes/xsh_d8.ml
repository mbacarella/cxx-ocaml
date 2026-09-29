module M = struct type t = int let compare = compare end
module F (X : Set.OrderedType) : Set.OrderedType with type t = X.t =
struct
  module A = Set.Make (X)
  type t = X.t
  let compare = X.compare
end
