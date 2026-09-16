let f x = match x () with exception Not_found -> 1 | y -> y
