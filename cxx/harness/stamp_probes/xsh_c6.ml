module F (X : Map.OrderedType) : Map.S with type key = X.t = struct
  module A = Map.Make (X)
  include Map.Make (X)
end
