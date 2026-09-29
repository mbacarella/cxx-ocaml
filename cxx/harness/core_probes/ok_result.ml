let r = Ok 3
let e = Error "x"
let f = function Ok x -> x | Error _ -> 0
let g = Result.map succ
