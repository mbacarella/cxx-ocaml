let f ?opt x = match opt with Some y -> x + y | None -> x
let l = List.map f [1; 2]
