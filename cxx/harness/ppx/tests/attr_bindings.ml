let f x =
  let unused = 1 in
  x
let g = let y = 2 in 3
[@@@warning "+26+27"]
let h z = let w = z in 0
