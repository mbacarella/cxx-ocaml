module F (X : Map.OrderedType) = struct
  include (Map.Make (X) : Map.S)
  let z = 0
end
