class foo = object (self : 'self)
  val data1 = "foo"
  val data2 = "bar"
  val data3 = 42L
  method test1 = data1 ^ data2
  method test2 = false
  method test3 = self#test1
  method test4 = data3
end

class bar = object (self : 'self)
  inherit foo as super
  val! data2 = "test5"
  val data4 = "test3"
  val data5 = "test4"
  method test1 =
    data1
  ^ data2
  ^ data4
  ^ data5
  ^ Int64.to_string self#test4
end

class foobar = object inherit foo end
