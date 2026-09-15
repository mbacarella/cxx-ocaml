module F (X : sig type t val compare : t -> t -> int end) =
struct
  module XS = Set.Make(X)
  module XM = Map.Make(X)
  type t = Set.Make(X).t
  type u = int Map.Make(X).t
end
