module type Ord = sig type t val compare : t -> t -> int end
module F (X : Ord) : Map.S with type key = X.t = struct
  include Map.Make (X)
end
