module F (X : Map.OrderedType) : Map.S with type key = X.t = struct
  include Map.Make (X)
end
