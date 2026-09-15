module F (X : Map.OrderedType) = struct
  module S : Map.S = Map.Make (X)
  module T = struct let y1 = 1 let y2 = 2 end
  let z = 0
end
