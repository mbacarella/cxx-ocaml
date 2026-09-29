let f : 'a. 'a -> 'a list = fun x -> [x]
let g : type a b. a -> b -> a * b = fun x y -> (x, y)
