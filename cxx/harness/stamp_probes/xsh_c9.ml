module F (X : Map.OrderedType) : Map.S = struct
  include Map.Make (X)
end
