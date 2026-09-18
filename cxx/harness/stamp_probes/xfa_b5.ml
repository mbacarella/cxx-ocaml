module F (X : Set.OrderedType) = Set.Make (X)
module M : sig end = struct
  module N = F (struct type t = int let compare = compare end) end
