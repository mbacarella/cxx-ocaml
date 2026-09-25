let o = object method id : 'a. 'a -> 'a = fun x -> x method k = 3 end
let v = o#id 1
