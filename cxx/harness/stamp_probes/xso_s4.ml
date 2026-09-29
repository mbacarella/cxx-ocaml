module type S = sig
  open Map.Make(Bool)
  type u = int t
end
let x = 1
