module F (X : Set.OrderedType) : Map.S with type key = X.t = struct
  include Map.Make (struct type t = X.t let compare = X.compare end)
end
