module type S = sig
  open Set.Make(Bool)
  open Map.Make(Bool)
end
let x = 1
