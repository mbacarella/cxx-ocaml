let v = let rec l = 1 :: l in List.hd l
let w = let rec f = fun x -> g x and g = fun x -> x in f 1
let z = let rec r = { contents = r } in r
