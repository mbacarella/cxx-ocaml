let v = let rec l = lazy (Lazy.force l + 1) in l
let w = let rec a = [| 1 |] and b = (a, 3) in b
