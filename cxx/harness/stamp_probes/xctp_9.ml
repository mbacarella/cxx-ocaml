class type c = object method m : 'a. 'a -> 'a end
let f (o : c) = o#m 1, o#m "s"
