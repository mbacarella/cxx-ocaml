let l = lazy (1 + 2)
let v = Lazy.force l
let f = function lazy x -> x
