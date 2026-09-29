module type S = sig
  open Map.Make(Bool)
  type u = key
end
let x = 1
