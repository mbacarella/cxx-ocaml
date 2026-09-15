module F (X : Map.OrderedType) : Map.S with type key = X.t = Map.Make (X)
